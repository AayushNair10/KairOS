// grpcserver is the latency-critical service: a gRPC server whose requests
// are executed by a fixed pool of worker threads, each request doing a
// calibrated amount of CPU work.
//
// Each worker goroutine is locked to its own OS thread, as in a thread-pool
// server: handing a request to a worker means waking that particular kernel
// thread, and the kernel scheduler under test decides when it gets a CPU.
// With plain goroutines, Go's runtime would often run the work on the
// handler's thread without any kernel wakeup.
//
// GOMAXPROCS stays at Go's default (number of CPUs), so at most that many
// workers run Go code at once. Raising it above the CPU count made each GC
// cycle start a mark worker on every idle Go "processor", i.e. many more GC
// threads than CPUs, and requests stalled for up to ~100 ms (measured
// 2026-10-29: p99 at 2000 req/s 76 ms with GOMAXPROCS=20, 0.9 ms with 4).
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"runtime/metrics"
	"sort"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
	"google.golang.org/grpc"

	pb "github.com/AayushNair10/KairOS/workloads/kairospb"
)

var (
	addr       = flag.String("addr", "127.0.0.1:50051", "listen address")
	workers    = flag.Int("workers", 16, "number of worker threads")
	workUs     = flag.Uint("work-us", 100, "CPU work per request (µs) when the request asks for 0")
	itersPerUs = flag.Float64("iters-per-us", 0, "work-loop iterations per µs; 0 = calibrate at startup")
	gomaxprocs = flag.Int("gomaxprocs", 0, "GOMAXPROCS; 0 = Go's default (number of CPUs)")
	calibrate  = flag.Bool("calibrate", false, "print the measured iterations per µs and exit")
	schedlat   = flag.Bool("schedlat", false, "log Go's goroutine scheduling latencies every second")
)

// sink receives the result of every spin() call, so the compiler can't
// treat the loop as dead code.
var sink atomic.Uint64

// spin does n iterations of a multiply-add. Each iteration depends on the
// previous result, so the loop can't be vectorised or skipped and runs at a
// steady speed. It touches no memory: the service's work is CPU-only.
func spin(n uint64) uint64 {
	h := uint64(1)
	for i := uint64(0); i < n; i++ {
		h = h*6364136223846793005 + i
	}
	return h
}

// measureItersPerUs times spin() and returns the fastest rate seen over a
// few trials, i.e. the one least disturbed by other work. Run it on an idle
// machine; the harness does this once per experiment batch so every run in
// the batch does the same amount of work per request.
func measureItersPerUs() float64 {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	const n = 2_000_000
	best := 0.0
	for trial := 0; trial < 20; trial++ {
		start := time.Now()
		sink.Add(spin(n))
		us := float64(time.Since(start).Nanoseconds()) / 1000
		best = max(best, n/us)
	}
	return best
}

// job is one request, handed from a gRPC handler to a worker.
type job struct {
	iters    uint64
	received time.Time   // when the handler got the request
	done     chan timing // the worker sends exactly one value
}

type timing struct {
	queueNs, serviceNs uint64
	end                time.Time // when the worker finished
}

// worker runs jobs until the jobs channel is closed.
func worker(jobs <-chan *job) {
	// Keep this goroutine on one OS thread for its whole life: while the
	// queue is empty the thread sleeps in the kernel (futex), and the
	// kernel wakes it when a job arrives.
	runtime.LockOSThread()
	nameThread("grpcworker")
	for j := range jobs {
		start := time.Now()
		sink.Add(spin(j.iters))
		end := time.Now()
		j.done <- timing{
			queueNs:   uint64(start.Sub(j.received)),
			serviceNs: uint64(end.Sub(start)),
			end:       end,
		}
	}
}

// nameThread sets the calling OS thread's name (its comm in the kernel),
// so a scheduler can tell worker threads from the gRPC and Go runtime
// threads, which keep the process name. The caller must be locked to its
// thread. The main thread is left alone: its name is the process's name.
func nameThread(name string) {
	if unix.Gettid() == unix.Getpid() {
		log.Printf("a worker runs on the main thread; not renamed")
		return
	}
	b, err := unix.BytePtrFromString(name)
	if err == nil {
		err = unix.Prctl(unix.PR_SET_NAME, uintptr(unsafe.Pointer(b)), 0, 0, 0)
	}
	if err != nil {
		log.Printf("naming worker thread: %v", err)
	}
}

// schedLatBoundsUs are the bucket bounds for logSchedLatency, the same as
// scx_kairos's --diag buckets (DIAG_BOUNDS_US).
var schedLatBoundsUs = []float64{50, 200, 1000, 2000, 5000, 10000, 20000}

// logSchedLatency logs, every second, how long goroutines waited to run
// after becoming runnable in that second: Go's /sched/latencies histogram,
// summed into schedLatBoundsUs buckets ("schedlat unix_ns=<now>
// counts=<lt50us>,...,<ge20000us>"). This is queueing inside the Go
// runtime, which the kernel can't see (a goroutine waits for a Go
// "processor"), plus the kernel wait of the thread that runs it. Go's own
// buckets are placed by their lower edge; they are much finer than these.
func logSchedLatency() {
	s := []metrics.Sample{{Name: "/sched/latencies:seconds"}}
	var prev []uint64
	for range time.Tick(time.Second) {
		metrics.Read(s)
		h := s[0].Value.Float64Histogram()
		out := make([]uint64, len(schedLatBoundsUs)+1)
		for i, c := range h.Counts {
			if i < len(prev) {
				c -= prev[i]
			}
			lowUs := h.Buckets[i] * 1e6 // -Inf for the first bucket
			k := sort.Search(len(schedLatBoundsUs), func(j int) bool { return schedLatBoundsUs[j] > lowUs })
			out[k] += c
		}
		prev = append(prev[:0], h.Counts...)
		parts := make([]string, len(out))
		for i, c := range out {
			parts[i] = fmt.Sprint(c)
		}
		log.Printf("schedlat unix_ns=%d counts=%s", time.Now().UnixNano(), strings.Join(parts, ","))
	}
}

type server struct {
	pb.UnimplementedWorkServer
	// The server's request queue: handlers put jobs here and idle workers
	// take them in FIFO order. A job waits here only while every worker is
	// busy. A woken worker then still needs one of GOMAXPROCS Go
	// "processors" and, finally, a CPU from the kernel.
	jobs       chan *job
	defaultUs  uint32
	itersPerUs float64
}

// Do is the gRPC handler. It runs on a goroutine created by gRPC, hands the
// work to a worker thread and waits for it to finish.
func (s *server) Do(ctx context.Context, req *pb.WorkRequest) (*pb.WorkReply, error) {
	us := req.GetWorkUs()
	if us == 0 {
		us = s.defaultUs
	}
	j := &job{
		iters:    uint64(float64(us) * s.itersPerUs),
		received: time.Now(),
		done:     make(chan timing, 1),
	}
	s.jobs <- j
	t := <-j.done
	// The worker's send made this goroutine runnable; how long until it ran?
	wake := time.Since(t.end)
	return &pb.WorkReply{Id: req.GetId(), QueueNs: t.queueNs, ServiceNs: t.serviceNs,
		RecvUnixNs: uint64(j.received.UnixNano()), WakeNs: uint64(wake)}, nil
}

func main() {
	// Wall-clock start, so GODEBUG=gctrace=1 lines ("@<seconds since
	// start>") can be placed on the load generator's time axis (to within
	// the few ms between runtime start and main).
	startNs := time.Now().UnixNano()
	flag.Parse()
	if *calibrate {
		fmt.Printf("%.1f\n", measureItersPerUs())
		return
	}

	ipu := *itersPerUs
	if ipu == 0 {
		ipu = measureItersPerUs()
	}
	if *gomaxprocs > 0 {
		runtime.GOMAXPROCS(*gomaxprocs)
	}
	procs := runtime.GOMAXPROCS(0) // 0 = just report the current value

	s := &server{
		jobs:       make(chan *job, 4096),
		defaultUs:  uint32(*workUs),
		itersPerUs: ipu,
	}
	for i := 0; i < *workers; i++ {
		go worker(s.jobs)
	}
	if *schedlat {
		go logSchedLatency()
	}

	lis, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	g := grpc.NewServer()
	pb.RegisterWorkServer(g, s)

	// SIGINT/SIGTERM (the harness sends SIGTERM): finish in-flight
	// requests, then Serve returns and the process exits.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		g.GracefulStop()
	}()

	log.Printf("listening on %s: workers=%d work_us=%d iters_per_us=%.1f gomaxprocs=%d start_unix_ns=%d",
		*addr, *workers, *workUs, ipu, procs, startNs)
	if err := g.Serve(lis); err != nil {
		log.Fatal(err)
	}
}
