// loadgen is an open-loop load generator for grpcserver.
//
// Open loop: requests are sent at times fixed in advance, whether or not
// earlier requests have been answered. A closed-loop client, which waits
// for each reply before sending more, slows down exactly when the server is
// slow and so hides the worst latencies ("coordinated omission").
//
// Arrivals are a Poisson process whose rate changes in phases, e.g.
// -phases 10s@500,20s@4000,10s@500 (quiet, burst, quiet). The schedule is
// computed up front from -seed, so every run sends at the same times.
//
// Output: one CSV row per request (-out) and a per-phase summary (-summary
// as JSON, and printed to stderr). Latency is measured from the *intended*
// send time, so lateness of the sender itself counts against latency; the
// actual send time is recorded too, so the two can be told apart.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"math/rand/v2"
	"os"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/sys/unix"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	pb "github.com/AayushNair10/KairOS/workloads/kairospb"
)

var (
	addr    = flag.String("addr", "127.0.0.1:50051", "server address")
	phasesF = flag.String("phases", "10s@1000", "load phases: comma-separated duration@requests_per_second")
	workUs  = flag.Uint("work-us", 0, "CPU work per request (µs); 0 = server default")
	conns   = flag.Int("conns", 8, "number of gRPC connections; requests are spread round-robin")
	seed    = flag.Uint64("seed", 1, "random seed for the arrival times")
	timeout = flag.Duration("timeout", 5*time.Second, "per-request deadline")
	outPath = flag.String("out", "requests.csv", "per-request CSV output")
	sumPath = flag.String("summary", "", "per-phase summary JSON output (optional)")
)

type phase struct {
	dur time.Duration
	rps float64
}

func parsePhases(s string) ([]phase, error) {
	var phases []phase
	for _, part := range strings.Split(s, ",") {
		durS, rpsS, ok := strings.Cut(strings.TrimSpace(part), "@")
		if !ok {
			return nil, fmt.Errorf("phase %q: want duration@rps", part)
		}
		dur, err := time.ParseDuration(durS)
		if err != nil {
			return nil, fmt.Errorf("phase %q: %v", part, err)
		}
		rps, err := strconv.ParseFloat(rpsS, 64)
		if err != nil || rps < 0 {
			return nil, fmt.Errorf("phase %q: bad rate", part)
		}
		phases = append(phases, phase{dur, rps})
	}
	return phases, nil
}

// schedule returns the intended send time of every request (ns after the
// start) and the phase it belongs to. Within a phase the gaps between
// requests are exponential with mean 1/rps, which makes the arrivals a
// Poisson process. At a phase boundary the next gap is drawn fresh with the
// new rate; that is exact because the exponential distribution is
// memoryless.
func schedule(phases []phase, seed uint64) (offs []int64, phaseOf []int) {
	rng := rand.New(rand.NewPCG(seed, 0))
	begin := 0.0 // start of the current phase, ns
	for pi, ph := range phases {
		end := begin + float64(ph.dur.Nanoseconds())
		if ph.rps > 0 {
			gap := func() float64 { return rng.ExpFloat64() / ph.rps * 1e9 }
			for t := begin + gap(); t < end; t += gap() {
				offs = append(offs, int64(t))
				phaseOf = append(phaseOf, pi)
			}
		}
		begin = end
	}
	return offs, phaseOf
}

// sleepUntil blocks the calling OS thread until CLOCK_MONOTONIC reaches
// target (ns). Measured on this VM (2026-10-29): time.Sleep(100µs) woke
// 1.2 ms late (median), which would bunch requests together; an absolute
// clock_nanosleep with 1 ns timer slack woke 60 µs late.
func sleepUntil(target int64) {
	ts := unix.NsecToTimespec(target)
	for unix.ClockNanosleep(unix.CLOCK_MONOTONIC, unix.TIMER_ABSTIME, &ts, nil) == unix.EINTR {
		// Interrupted by a signal (the Go runtime sends some); with an
		// absolute deadline, just sleep again.
	}
}

// record is what we learn about one request. All times are ns after start.
type record struct {
	sentNs, doneNs     int64
	queueNs, serviceNs uint64 // measured by the server
	code               codes.Code
}

func send(c pb.WorkClient, id int, t0 time.Time, r *record) {
	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()
	rep, err := c.Do(ctx, &pb.WorkRequest{Id: uint64(id), WorkUs: uint32(*workUs)})
	r.doneNs = int64(time.Since(t0))
	if err != nil {
		r.code = status.Code(err)
		return
	}
	r.queueNs, r.serviceNs = rep.GetQueueNs(), rep.GetServiceNs()
}

// warmup waits until the server answers and opens every connection, so
// server startup and connection setup don't land in the measurement.
func warmup(clients []pb.WorkClient) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for _, c := range clients {
		for range 3 {
			req := &pb.WorkRequest{WorkUs: uint32(*workUs)}
			if _, err := c.Do(ctx, req, grpc.WaitForReady(true)); err != nil {
				return err
			}
		}
	}
	return nil
}

func main() {
	flag.Parse()
	phases, err := parsePhases(*phasesF)
	if err != nil {
		log.Fatal(err)
	}
	offs, phaseOf := schedule(phases, *seed)

	clients := make([]pb.WorkClient, *conns)
	for i := range clients {
		cc, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			log.Fatal(err)
		}
		defer cc.Close()
		clients[i] = pb.NewWorkClient(cc)
	}
	if err := warmup(clients); err != nil {
		log.Fatalf("server not reachable at %s: %v", *addr, err)
	}

	// The sender loop runs on its own OS thread, so the blocking
	// clock_nanosleep and the timer-slack setting affect only it.
	runtime.LockOSThread()
	if err := unix.Prctl(unix.PR_SET_TIMERSLACK, 1, 0, 0, 0); err != nil {
		log.Fatal(err)
	}
	var mono unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &mono); err != nil {
		log.Fatal(err)
	}
	base := mono.Nano() // start on the CLOCK_MONOTONIC scale, for sleepUntil
	t0 := time.Now()    // the same instant, for measuring with time.Since
	cpu0 := cpuTime()

	recs := make([]record, len(offs))
	var wg sync.WaitGroup
	for i, off := range offs {
		sleepUntil(base + off)
		recs[i].sentNs = int64(time.Since(t0))
		wg.Add(1)
		go func() {
			defer wg.Done()
			send(clients[i%len(clients)], i, t0, &recs[i])
		}()
	}
	wg.Wait()
	runtime.UnlockOSThread()
	cpuS, wallS := cpuTime()-cpu0, time.Since(t0).Seconds()

	if err := writeCSV(*outPath, offs, phaseOf, recs); err != nil {
		log.Fatal(err)
	}
	sum := summarize(phases, offs, phaseOf, recs, t0)
	sum.Gomaxprocs = runtime.GOMAXPROCS(0)
	sum.LoadWallS, sum.LoadCPUS = wallS, cpuS
	printSummary(sum)
	if *sumPath != "" {
		b, err := json.MarshalIndent(sum, "", "  ")
		if err != nil {
			log.Fatal(err)
		}
		if err := os.WriteFile(*sumPath, append(b, '\n'), 0o644); err != nil {
			log.Fatal(err)
		}
	}
}

func writeCSV(path string, offs []int64, phaseOf []int, recs []record) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	w := bufio.NewWriter(f)
	fmt.Fprintln(w, "id,phase,intended_ns,sent_ns,done_ns,latency_ns,queue_ns,service_ns,code")
	for i, r := range recs {
		fmt.Fprintf(w, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, phaseOf[i], offs[i],
			r.sentNs, r.doneNs, r.doneNs-offs[i], r.queueNs, r.serviceNs, r.code)
	}
	if err := w.Flush(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

type phaseSummary struct {
	Phase       int     `json:"phase"`
	DurationS   float64 `json:"duration_s"`
	TargetRPS   float64 `json:"target_rps"`
	StartUnixNs int64   `json:"start_unix_ns"`
	EndUnixNs   int64   `json:"end_unix_ns"`
	Requests    int     `json:"requests"`
	Errors      int     `json:"errors"`
	OkRPS       float64 `json:"ok_rps"` // successful requests / phase duration
	// Latency = reply time - intended send time, successful requests only.
	LatP50Us  float64 `json:"lat_p50_us"`
	LatP99Us  float64 `json:"lat_p99_us"`
	LatP999Us float64 `json:"lat_p999_us"`
	LatMaxUs  float64 `json:"lat_max_us"`
	// How late the sender was: actual - intended send time.
	SendLagP99Us float64 `json:"send_lag_p99_us"`
}

type summary struct {
	Addr        string         `json:"addr"`
	PhasesSpec  string         `json:"phases_spec"`
	WorkUs      uint           `json:"work_us"`
	Conns       int            `json:"conns"`
	Seed        uint64         `json:"seed"`
	StartUnixNs int64          `json:"start_unix_ns"`
	Phases      []phaseSummary `json:"phases"`
	// The load generator's own CPU use (user + system) while sending and
	// waiting for replies, to check that it isn't short of CPU.
	Gomaxprocs int     `json:"gomaxprocs"`
	LoadWallS  float64 `json:"load_wall_s"`
	LoadCPUS   float64 `json:"load_cpu_s"`
}

// cpuTime returns this process's user + system CPU time so far, in seconds.
func cpuTime() float64 {
	var ru unix.Rusage
	if err := unix.Getrusage(unix.RUSAGE_SELF, &ru); err != nil {
		log.Fatal(err)
	}
	return float64(ru.Utime.Nano()+ru.Stime.Nano()) / 1e9
}

// pct returns the q-quantile (0 < q <= 1) of sorted, by nearest rank.
func pct(sorted []int64, q float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	i := int(math.Ceil(q*float64(len(sorted)))) - 1
	return sorted[max(i, 0)]
}

func summarize(phases []phase, offs []int64, phaseOf []int, recs []record, t0 time.Time) summary {
	sum := summary{Addr: *addr, PhasesSpec: *phasesF, WorkUs: *workUs, Conns: *conns,
		Seed: *seed, StartUnixNs: t0.UnixNano()}
	lat := make([][]int64, len(phases))
	lag := make([][]int64, len(phases))
	errs := make([]int, len(phases))
	n := make([]int, len(phases))
	for i, r := range recs {
		p := phaseOf[i]
		n[p]++
		lag[p] = append(lag[p], r.sentNs-offs[i])
		if r.code != codes.OK {
			errs[p]++
			continue
		}
		lat[p] = append(lat[p], r.doneNs-offs[i])
	}
	begin := int64(0)
	for p, ph := range phases {
		slices.Sort(lat[p])
		slices.Sort(lag[p])
		us := func(ns int64) float64 { return float64(ns) / 1000 }
		sum.Phases = append(sum.Phases, phaseSummary{
			Phase:        p,
			DurationS:    ph.dur.Seconds(),
			TargetRPS:    ph.rps,
			StartUnixNs:  t0.UnixNano() + begin,
			EndUnixNs:    t0.UnixNano() + begin + ph.dur.Nanoseconds(),
			Requests:     n[p],
			Errors:       errs[p],
			OkRPS:        float64(len(lat[p])) / ph.dur.Seconds(),
			LatP50Us:     us(pct(lat[p], 0.50)),
			LatP99Us:     us(pct(lat[p], 0.99)),
			LatP999Us:    us(pct(lat[p], 0.999)),
			LatMaxUs:     us(pct(lat[p], 1)),
			SendLagP99Us: us(pct(lag[p], 0.99)),
		})
		begin += ph.dur.Nanoseconds()
	}
	return sum
}

func printSummary(sum summary) {
	fmt.Fprintf(os.Stderr, "%5s %7s %8s %8s %6s %9s %9s %9s %10s %11s\n", "phase", "dur_s",
		"target", "ok_rps", "errors", "p50_us", "p99_us", "p99.9_us", "max_us", "lag_p99_us")
	for _, p := range sum.Phases {
		fmt.Fprintf(os.Stderr, "%5d %7.1f %8.0f %8.0f %6d %9.0f %9.0f %9.0f %10.0f %11.0f\n",
			p.Phase, p.DurationS, p.TargetRPS, p.OkRPS, p.Errors,
			p.LatP50Us, p.LatP99Us, p.LatP999Us, p.LatMaxUs, p.SendLagP99Us)
	}
	fmt.Fprintf(os.Stderr, "loadgen: %.2f s CPU in %.1f s (%.0f%% of one CPU), GOMAXPROCS=%d\n",
		sum.LoadCPUS, sum.LoadWallS, 100*sum.LoadCPUS/sum.LoadWallS, sum.Gomaxprocs)
}
