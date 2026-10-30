# Research Log: KairOS — Load-Adaptive Wakeup Preemption with sched_ext

CSE 506 (Fall 2026) individual project. Author: Aayush Nair.

This file is the single running record of the project: decisions, related work,
design, experiments, results and open questions. It is organized so each section
feeds a section of the final report. Add to it after every working session.

**Rules for this log**
- Append; don't silently rewrite history. If a decision changes, add a new entry
  in the Decision Log saying what changed and why.
- Every result must point to its raw data (path under `results/`) and the git
  commit that produced it.
- Separate what was *measured* from what is *believed*. Mark guesses as such.

Report sections each part feeds:

| Log section | Report section |
|---|---|
| 1. Goal and hypothesis | Introduction |
| 2. Related work | Background |
| 3. Design | Design and implementation |
| 4. Implementation notes | Design and implementation |
| 5. Experiment log, 6. Results | Evaluation |
| 7. Explanations and insights | Discussion |
| 8. Limitations | Discussion |
| 9. Open questions | (working notes) |
| 10. Session entries | (working notes; source material for everything) |

---

## 0. Deadlines

| Deliverable | Due | Weight | Limit |
|---|---|---|---|
| Proposal | Fri Oct 30, 2026 | 1% | 2 pages |
| Progress report (needs screenshot of own BPF callback running) | Fri Nov 13, 2026 | 4% | 3 pages |
| Video demo (Zoom, live run) | Fri Dec 11, 2026 | 5% | 5 minutes |
| Final report (USENIX / OSDI '27 template, with code + build/run instructions) | Fri Dec 11, 2026 | 10% | 5 pages |

Grading emphasis: explain *why* the scheduler behaves as observed. A simple,
well-tested scheduler with insightful analysis beats a complex one with weak evidence.

---

## 1. Goal and hypothesis

**Goal.** Build a sched_ext scheduler that adapts *wakeup preemption* to current
and predicted load, so that a latency-critical service colocated with batch work
gets low tail latency when the system is quiet and during bursts, while batch work
keeps high throughput, and no task starves.

**Hypothesis.** No fixed preemption policy is best at all load levels:
- At low load, preempting on wakeup is cheap and gives immediate service.
- At high load, each preemption costs a context switch, cache/TLB pollution and
  broken batching, and delays everyone queued; letting the running task finish
  its slice gives better throughput and can give better tail latency.
A scheduler that switches modes using smoothed (and optionally predicted) load can
match the better fixed policy in each phase of a bursty workload. Its benefit
depends on burst duration relative to its reaction time.

**Intended contributions**
1. Load-adaptive wakeup preemption: two time scales of smoothing, asymmetric
   hysteresis ("fast exit, cautious entry").
2. Feature A: dependency-based task classification (latency-critical = others
   depend on it, not merely "sleeps a lot").
3. Feature B: CPU budget for latency-critical tasks + maximum-wait guarantee for
   every task (anti-starvation).
4. User-space ML: burst prediction (primary) and task classification (secondary),
   kept off the scheduling critical path.
5. Evaluation on synthetic and real (Azure Functions 2021) burst patterns,
   explaining when adaptivity helps.

**Minimum deliverable** if time runs short: adaptive preemption + A + B.
ML is an extension.

---

## 2. Related work (Background)

### 2.1 Schedulers and their preemption / timeslice philosophy

| System | Wakeup preemption | Timeslice | Notes |
|---|---|---|---|
| Linux EEVDF (default since 6.6) | Preempts if the woken task's virtual deadline is earlier. RUN_TO_PARITY protects the running task's slice. `SCHED_BATCH` disables wakeup preemption statically | Base slice; since 6.12 tasks may request 100 µs–100 ms via `sched_setattr()` | Attempts to protect running tasks from wakeup preemption (RUN_TO_PARITY_WAKEUP, 2024 and July 2026) caused starvation under some cgroup layouts and were not merged |
| Linux CFS (pre-6.6) | Wakeup preemption with a fixed granularity threshold | Period stretched with number of tasks | Battle of the Schedulers: >2M preemptions of Apache's injector on one core |
| FreeBSD ULE | User tasks not preempted on wakeup | — | Apache ~39% faster than CFS on one core; but interactive class has absolute priority and can starve batch work (80-thread experiment) |
| scx_bpfland | No wakeup preemption of running task; woken task goes to idle CPU or waits in deadline-ordered queue | `slice_max / nr_waiting`, min floor; default max 1 ms | Prioritizes frequent voluntary sleepers; disables interactive boost under heavy backlog |
| scx_flash | No wakeup preemption described; sync-wakeup places wakee on waker's CPU | `slice_max / nr_waiting`, min 128 µs, max 4 ms | Renews running task's slice if nothing waits |
| scx_lavd | Only top ~1.5% latency-critical tasks preempt; victim CPU chosen by power-of-two random choices; preempts by setting victim slice to 1 ns (no IPI) | 20 ms target latency × active CPUs / queued tasks, EWMA-smoothed; 3× boost for long runners when underloaded | Latency-criticality from wakeup frequency and waker/wakee relations; core compaction for power |

**Gap.** Load-adaptive *timeslices* already exist (bpfland, flash, lavd), and they
all *shrink* slices as load rises. No documented sched_ext scheduler explicitly
switches wakeup preemption on/off by measured load with hysteresis and analyzes
the crossover. (Checked design notes, not every line of source; re-verify before
the final report.)

### 2.2 Preemption cost and microsecond scheduling
- Shinjuku (Kaffes et al., NSDI 2019): frequent µs-scale preemption cuts tail latency for mixed request sizes.
- Concord (Iyer et al., SOSP 2023): approximates single-queue + preemption more cheaply; better throughput under tail-latency SLOs. Preemption frequency is a cost–benefit trade-off.
- FlexSC (Soares & Stumm, OSDI 2010): indirect cost (cache/TLB/branch predictor pollution) of mode switches exceeds the direct cost.

### 2.3 Load-based mode switching (analogy)
- Hybrid interrupts/polling (Salah et al., ~2007): interrupts at light load, polling at heavy load; switch just before the "cliff point".
- Linux NAPI: per-packet interrupts at low load, batched polling at high load.

### 2.4 Identifying latency-critical tasks
- Etsion, Tsafrir, Feitelson (NOSSDAV 2004): CPU-usage metrics failed to separate multimedia from batch; user-I/O (keyboard/mouse/screen) worked.
- Criticality Stacks (Du Bois et al., ISCA 2013): a thread is critical if its progress determines the program's progress; running threads accrue t/r per interval while others wait.
- BIS (Joao et al., ASPLOS 2012): identify bottlenecks by cycles others spend waiting on them.
- Thread Criticality Predictors (Bhattacharjee & Martonosi, ISCA 2009).
- Linux `wake_wide()` / `wakee_flips`: detects 1:N waker/wakee patterns.
- Takeaway: "others depend on it" is a better signal than "sleeps a lot".

### 2.5 Colocation
- Heracles (Lo et al., ISCA 2015), PARTIES (ASPLOS 2019), Caladan (Fried et al., OSDI 2020): latency-critical + best-effort sharing, mostly via resource/core partitioning.
- scx_layered (Meta): manual config assigning tasks to layers.

### 2.6 Traffic predictability (for the ML part)
- Leland et al. (SIGCOMM 1993): traffic is bursty at all time scales (self-similar); short-scale bursts are hard to predict.
- Shahrad et al., Serverless in the Wild (USENIX ATC 2020): Azure Functions characterization; many functions rarely invoked; frequencies span ~8 orders of magnitude.
- Expectation: periodic/daily patterns predictable; burst *continuation* partly predictable; random burst *onset* at ms scale mostly not.

### 2.7 ML in the kernel path
- LiteFlow (SIGCOMM 2022): small models in kernel networking path. Our design keeps the model in user space (PageFlex-style), so this is context only.

### 2.8 Course papers to cite
Battle of the Schedulers (ATC 2018), ghOSt (SOSP 2021), FlexSC (OSDI 2010),
PageFlex (ATC 2025), Linux scalability (OSDI 2010), Multikernel (SOSP 2009).

### 2.9 Links
- sched_ext docs: https://docs.kernel.org/scheduler/sched-ext.html
- scx repository: https://github.com/sched-ext/scx
- bpfland notes: https://gitea.hillion.co.uk/JakeHillion/scx-upstream/src/branch/experimental-scheds/ai_docs/SCX_BPFLAND.md
- flash notes: https://gitea.hillion.co.uk/JakeHillion/scx-upstream/raw/branch/experimental-scheds/ai_docs/SCX_FLASH.md
- lavd notes: https://gitea.hillion.co.uk/JakeHillion/scx-upstream/raw/branch/experimental-scheds/ai_docs/SCX_LAVD.md
- LWN, Completing the EEVDF scheduler: https://lwn.net/Articles/969062
- RUN_TO_PARITY_WAKEUP starvation (Jul 2026): https://patchew.org/linux/20260712113106.2564384-1-chenjinghuang2@huawei.com/
- EEVDF shorter slices wakeup-preempt: https://lkml.iu.edu/hypermail/linux/kernel/2408.0/04036.html
- Shinjuku: https://www.usenix.org/conference/nsdi19/presentation/kaffes
- Concord: https://dslab.epfl.ch/research/concord
- QoS-aware dynamic time-slice tuning (IISWC 2014): https://pure.psu.edu/en/publications/qos-aware-dynamic-time-slice-tuning/
- Salah et al.: https://eprints.kfupm.edu.sa/69/1/salah_revised.pdf
- Etsion et al.: https://cris.huji.ac.il/en/publications/desktop-scheduling-how-can-we-know-what-the-user-wants/
- Criticality Stacks: https://soft.vub.ac.be/~jsartor/researchDocs/thread_critical.pdf
- BIS: https://pdl.cmu.edu/PDL-FTP/associated/bottleneck-id_Mutlu_asplos12_abs.shtml
- Thread Criticality Predictors: https://mrmgroup.cs.princeton.edu/papers/abhattac-isca2009.pdf
- wake_wide patch: https://lkml.iu.edu/hypermail/linux/kernel/1507.1/00039.html
- Leland et al.: https://web.stanford.edu/class/cs244/papers/selfsimilar.pdf
- Serverless in the Wild: https://www.usenix.org/conference/atc20/presentation/shahrad
- Azure Public Dataset: https://github.com/Azure/AzurePublicDataset
- k6 ramping-arrival-rate: https://grafana.com/docs/k6/latest/using-k6/scenarios/executors/ramping-arrival-rate/
- ghz: https://pkg.go.dev/github.com/bojand/ghz@v0.114.0

---

## 3. Design

### 3.1 Overview
The BPF scheduler makes every decision in-kernel. A periodic BPF timer maintains
load averages and the mode. User space only loads the scheduler, logs statistics,
and (optionally) writes ML predictions/parameters into BPF maps every few seconds.
Stale or missing ML output is ignored (reactive fallback).

### 3.2 Modes
- LATENCY mode: short slices; a woken task may preempt a CPU running a lower class.
- BATCH mode: no wakeup preemption except latency-critical tasks within budget;
  batch/background get long, protected slices.

### 3.3 Load signal
`load = runnable tasks / nr_cpus` (×1024 fixed-point). Kept as fast EWMA
(shift ~2–3) and slow EWMA (shift ~5–6). Wakeup rate used as a leading indicator;
queue wait time used to confirm impact. With ≤ 1 runnable per CPU, the mode is
irrelevant (every task has a CPU), so experiments must oversubscribe.

Integer EWMA: `avg = avg - (avg >> k) + (sample >> k)`.

### 3.4 Mode decision tree (BPF timer, every 1–5 ms)
```
if fast_load < LOW:                              -> LATENCY   (fast exit)
elif ML burst prediction fresh and confident:    -> BATCH     (pre-armed)
elif fast_load > HIGH and slow_load > HIGH:      -> BATCH     (confirmed)
else:                                            -> keep mode (hysteresis)
```
Initial guesses: LOW = 1.0, HIGH = 2.0 runnable per CPU (calibrate from E1).
ML can only switch *into* BATCH early; the quiet check always wins.

### 3.5 Feature A: task classification tree (run at `quiescent`)
Per-task features (task storage, integers): `nsamples`, `vol_ratio` (/1024),
`avg_burst_us`, `cpu_share` (/1024), `wakes_others_per_s`, `woken_by_event`
(/1024), `periodicity` (interval deviation / mean, /1024).
```
nsamples < 16                                   -> NORMAL (unknown)
vol_ratio < 512                                 -> BATCH (mostly preempted)
cpu_share > 600                                 -> BATCH (CPU hog)
periodicity < 100 and wakes_others_per_s < 5    -> BACKGROUND (timer job)
wakes_others_per_s >= 20 or woken_by_event > 700:
    avg_burst_us <= 500                         -> LATENCY_CRITICAL
    else                                        -> NORMAL (heavy handler)
else                                            -> NORMAL
```
All thresholds are placeholders. Labels persist while a task is idle (so the next
burst doesn't start with relearning).

| Class | LATENCY mode | BATCH mode |
|---|---|---|
| LATENCY_CRITICAL | short slice, may preempt | short slice, may preempt batch (within budget) |
| NORMAL | short slice, may preempt | normal slice, no preemption |
| BATCH | normal slice, no preemption | long protected slice |
| BACKGROUND | long slice, lowest priority | long slice, lowest priority |

### 3.6 Feature B: budget and starvation guard
- Budget: if latency-critical CPU time in the window (e.g., 100 ms) > BUDGET
  (e.g., 70%), latency-critical tasks lose preemption rights until window resets.
  When the budget is exhausted the scheduler behaves like a fair scheduler.
- Max wait: any task waiting > MAX_WAIT (e.g., 50 ms) runs next, regardless of mode
  or class. Must stay far below the sched_ext watchdog timeout.

### 3.7 Callback responsibilities
| Callback | Job |
|---|---|
| init_task | create task storage, class = NORMAL |
| runnable | runnable_at; wakeup interval/periodicity; credit waker; count wakeup |
| select_cpu | idle CPU → insert directly |
| enqueue | main decision (mode × class × budget) |
| dispatch | overdue tasks first; else shared queue by vtime |
| running | started_at; record wait; per-CPU current class |
| stopping | charge runtime; voluntary vs involuntary; budget accounting |
| quiescent | close burst; update features; reclassify |
| timer | EWMAs; mode tree; budget window reset; ML freshness |

### 3.8 ML (user space, Python)
- Burst predictor (primary): input = scheduler-measured load history from stats map
  (never the load generator's schedule). Output = P(load > HIGH next window) +
  timestamp → params map. Model: tree / boosted trees. **Must compare against
  persistence ("next = current") and moving average.**
- Task classifier (secondary): input = per-task feature snapshots via ring buffer;
  labels from known roles in training runs; never use names as features; depth ≤ 5
  tree; output = per-task class overrides with timestamp.
- Train/test on different patterns and services.

---

## 4. Implementation notes
(Kernel version, API names actually used, verifier problems and fixes, mechanism
choices. Add as you go.)

- Environment plan: MacBook Air M2 (4P+4E, 8 GB, fanless). Ubuntu Server ARM64
  (25.04+), VM with 4 vCPUs, 4 GB RAM, 40 GB disk. sched_ext needs kernel ≥ 6.12
  with CONFIG_SCHED_CLASS_EXT=y.
- To decide early: preemption mechanism (SCX_ENQ_PREEMPT insert vs
  scx_bpf_kick_cpu(..., SCX_KICK_PREEMPT) vs shrinking victim slice).
- To verify early: whether the waker can be identified reliably in `runnable`
  (including wakeups from interrupt context).
- Kfunc names changed across versions (e.g., scx_bpf_dispatch → scx_bpf_dsq_insert
  in 6.13). Follow the example schedulers built for the installed kernel.
- Setup choices (2026-10-26): Lima VM (`template://ubuntu-lts`) running Ubuntu
  26.04 LTS, which ships Linux 7.0. The scx repo's install guide supports Ubuntu
  25.04 and newer. Rust schedulers (scx_bpfland, scx_lavd) build with cargo from
  github.com/sched-ext/scx (needs clang ≥ 16, libbpf ≥ 1.2.2, Rust ≥ 1.91). C
  examples such as scx_simple are no longer in the scx repo's main tree; build
  them from the matching kernel source's `tools/sched_ext/` with make.
- Baselines built from sched-ext/scx commit
  4c63759101a78d3595e8ec27c8c9c5aaac43b1fe. Claude Code 2.1.311 in the VM.
- Claude Code runs inside the VM (native installer). Lima gives passwordless sudo,
  so Claude Code's permission prompts are the safety net for root commands.
- Build (2026-10-28): `make -C scheduler`. `scheduler/Makefile` repeats the
  compile and skeleton rules of `~/linux-src/tools/sched_ext/Makefile` (whose
  rules only find sources in its own directory) and reuses that build's
  `libbpf.a`, libbpf headers, `bpftool` and `vmlinux.h`, plus the scx headers
  from `tools/sched_ext/include`. Steps: clang `-target bpf` → `bpftool gen
  object` (one link pass; upstream links three times as a linker self-test) →
  `bpftool gen skeleton` → cc links the loader with static libbpf. The upstream
  build must exist first. `vmlinux.h` was dumped from the running kernel's BTF
  (`/sys/kernel/btf/vmlinux`), so it describes Ubuntu's 7.0.0-38 kernel rather
  than vanilla v7.0; rerun the upstream build after a kernel update.
- sched_ext API used by the scx_simple code on 7.0: `scx_bpf_select_cpu_dfl`,
  `scx_bpf_dsq_insert` (to `SCX_DSQ_LOCAL`), `scx_bpf_dsq_insert_vtime`,
  `scx_bpf_dsq_move_to_local`, `scx_bpf_create_dsq`, `BPF_STRUCT_OPS`,
  `SCX_OPS_DEFINE`. `scx_bpf_dsq_insert_vtime` and `scx_bpf_dsq_move_to_local`
  are wrappers in `include/scx/compat.bpf.h` that pick whichever kfunc the
  running kernel has (`bpf_core_type_exists` / `bpf_ksym_exists`, resolved by
  libbpf at load). This kernel's BTF has `struct scx_bpf_dsq_insert_vtime_args`,
  so the args-struct kfunc `__scx_bpf_dsq_insert_vtime` is the one used.
  `SCX_SLICE_DFL` = 20 ms on this kernel.
- `bpf_printk` with ≤ 3 arguments compiles to helper 6 (`bpf_trace_printk`). In
  7.0 it formats into a per-CPU buffer and writes to the trace ring buffer (no
  global lock; read in `kernel/trace/bpf_trace.c`, `kernel/bpf/helpers.c`). Its
  cost is still paid on every enqueue, so remove or gate it before measuring.
- Slice mechanics (read in `kernel/sched/ext.c`, v7.0): the kernel subtracts
  run time from `p->scx.slice` (`update_curr_scx`) and reschedules at the tick
  once it reaches 0 (`task_tick_scx`). A still-runnable task whose slice ran out
  goes through `stopping(p, true)` and then `ops.enqueue` with enq_flags 0. If
  dispatch finds nothing else to run, the kernel keeps running prev and refills
  its slice to the default *without* calling `stopping`, so scx_simple's
  `SCX_SLICE_DFL - p->scx.slice` charge misses that earlier slice. For our
  runtime and budget accounting (Feature B), use timestamps (`running` →
  `stopping`) instead. Task weight is cgroup-style: nice 0 = 100.
- Idle CPU search (`scx_select_cpu_dfl`, `kernel/sched/ext_idle.c`): the kernel
  keeps a bitmask of idle CPUs, updated when a CPU enters or leaves idle; the
  search reads that mask and claims a CPU with an atomic test-and-clear. It
  never inspects the CPUs themselves. On this VM (no SMT, one LLC = CPUs 0–3,
  one NUMA node) the SMT/LLC/NUMA steps are disabled, so it is: sync-wakeup
  check, test prev_cpu's bit, search one 4-bit mask. When every CPU is busy the
  mask is empty and the search fails after a few bit operations. If we don't
  implement `ops.select_cpu`, the kernel runs this same search itself
  (`select_task_rq_scx`). Already-idle CPUs don't look at our shared queue
  until woken, so skipping the search would need `scx_bpf_kick_cpu` from
  enqueue (same search + IPI) or would leave CPUs idle. Cost not measured yet;
  for E7, `sysctl kernel.bpf_stats_enabled=1` + `bpftool prog show` should give
  run_time_ns / run_cnt per callback (unverified for struct_ops programs).
- `enq_flags` seen in `ops.enqueue` on 7.0 (core `ENQUEUE_*` bits in
  `kernel/sched/sched.h` plus SCX bits): `0x100009` = WAKEUP | NOCLOCK |
  RQ_SELECTED (= SCX_ENQ_CPU_SELECTED: select_cpu ran, found no idle CPU);
  `0x9` = wakeup of a task pinned to one CPU (kernel skips select_cpu, e.g.
  per-CPU kworkers); `0x80008` = INITIAL | NOCLOCK (newly forked task); `0x0` =
  still-runnable task re-enqueued after using up its slice.
- Stopping the scheduler from scripts: send SIGINT to the loader itself
  (`sudo pkill -INT -x scx_kairos`); that exits 0 and prints
  `EXIT: unregistered from user space`. Don't wrap it in `sudo timeout -s INT`:
  the scheduler still unregisters cleanly, but the loader is then killed by
  SIGKILL (exit 137) before printing its exit reason. `sudo` here is sudo-rs
  0.2.13.
- Workloads (2026-10-29): Go 1.26.8, grpc-go v1.85.0, protobuf-go v1.36.12;
  protoc 3.21.12 (apt), protoc-gen-go v1.36.12, protoc-gen-go-grpc v1.6.2.
  Generated code in `workloads/kairospb/` is committed; `make -C workloads`
  builds `bin/grpcserver` and `bin/loadgen`, `make -C workloads proto`
  regenerates.
- grpcserver: a handler puts each request on a job channel; N worker
  goroutines, each `LockOSThread`ed, so handing over a request wakes a
  particular kernel thread (thread-pool server). GOMAXPROCS stays at Go's
  default (4). With GOMAXPROCS=20, Go's GC ran mark workers on idle Ps, i.e.
  many more GC threads than CPUs: p99 at 2000 req/s was 76 ms, vs 3.4 ms with
  GOGC=off and 0.9 ms with GOMAXPROCS=4 (idle VM, no batch, one 6 s trial
  each; raw output not kept). Work = dependent multiply-add loop, calibrated
  at ~970–1030 iterations/µs (idle). The reply carries the server's
  queue_ns (handler → worker start) and service_ns (worker start → end).
- loadgen: open loop; the arrival schedule is computed up front from a seed
  (Poisson within each phase, next gap drawn fresh at a phase boundary). The
  sender runs on a locked OS thread using `clock_nanosleep(TIMER_ABSTIME)`
  with `PR_SET_TIMERSLACK` = 1 ns. Measured on the idle VM (2000 sleeps
  each): `time.Sleep(100µs)` p50 1.2 ms late (p99 1.6 ms); clock_nanosleep
  100 µs p50 60 µs late (p99 74 µs), 2 ms p50 0.5 ms late (guess: waking an
  idle vCPU is slow on the host). Latency = reply − intended send time; the
  actual send time is recorded too.
- Batch: `zstd -q -T1 -3` in a loop over the first 64 MB of `git archive` of
  `~/linux-src` (028ef9c96, v7.0): ratio 4.5×, ~238 MB/s per job when alone.
  zstd runs in the background and the loop `wait`s, so a TERM trap can kill
  it. (Stopping via `setsid` + process-group kill failed: setsid forks when
  started from a shell with job control, and the loop kept running.)
- Harness: `experiments/run.sh <conf>`. Per run: start the scheduler
  ("eevdf" = none), write meta.json, start server and batch jobs, warm up,
  run loadgen, stop everything, print `analysis/summarize_run.py` (per-phase
  latency and batch MB/s, each zstd pass prorated by its overlap with the
  phase). Work per request is calibrated once per batch, before the first
  run. `scripts/meta.sh` now also records `sched_ext_ops`.
- CPU pinning (2026-10-29, from N1 on): `run.sh` runs the server and batch
  jobs on CPUs 0–2 (`SUT_CPUS`) and the load generator on CPU 3
  (`LOADGEN_CPUS`), via `taskset` (it execs, so `$!` is still the program's
  PID). The load generator is still scheduled by the scheduler under test,
  but only against its own threads and stray system tasks. It needs
  GOMAXPROCS > 1: Go sets 1 on a one-CPU mask, and then the sender thread,
  asleep in `clock_nanosleep`, holds the only P so replies wait. Single 10 s
  trials, EEVDF, 4 zstd, 3000 req/s: send lag p99 17 ms unpinned
  (GOMAXPROCS 4), 38 ms pinned with GOMAXPROCS 1, 4 ms pinned with 4. The
  loadgen used 26–38% of CPU 3 at 3000 req/s.
- Each run now also records every process's CPU time around the load
  (`procs_before/after`), the load generator's own CPU use (loadgen.json),
  and the server's GC cycles (`GODEBUG=gctrace=1` in server.log, plus the
  server's start time so they can be placed in time). Tools:
  `analysis/aggregate.py` (median/range/CV across reps),
  `analysis/episodes.py` (slow episodes vs GC). `meta.sh`'s `git_dirty`
  now ignores `results/`; N2 and N3 say dirty only because of an untracked
  `analysis/__pycache__/` (now in `.gitignore`).
- Phase 4 scheduler (bd50425), kernel 7.0 API used: `ops.runnable` /
  `ops.quiescent` (bracket runnable time; skipped for CPU-to-CPU moves, and
  also called around attribute changes, so the count stays paired),
  `BPF_MAP_TYPE_TASK_STORAGE` + `bpf_task_storage_get` (created in
  `ops.init_task`), `scx_bpf_now()` (rq clock; may differ slightly between
  CPUs, so negative differences are skipped), `bpf_timer_*` on an array map
  as in scx_central (started in `ops.init`, re-armed in the callback, not
  pinned), `scx_bpf_nr_cpu_ids`, `scx_bpf_task_cpu`,
  `bpf_cpumask_test_cpu(cpu, p->cpus_ptr)`, `is_migration_disabled()`
  (common.bpf.h). `NR_STATS` clashes with an enum in vmlinux.h, hence
  `KAIROS_NR_STATS`.
- Preemption mechanism: insert the woken task with
  `scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, slice, enq_flags |
  SCX_ENQ_PREEMPT)`. In 7.0 (`local_dsq_post_enq`) this sets the target
  CPU's current task's slice to 0 and calls `resched_curr` (IPI), and the
  woken task is put at the head of that CPU's local queue. The preempted
  task goes through `stopping(p, true)` and, with slice 0,
  `ops.enqueue(p, 0)` (`put_prev_task_scx`), so it returns to the shared
  queue with the time it ran charged. Moving a migration-disabled task to
  another CPU this way makes the kernel stop the scheduler
  (`task_can_run_on_remote_rq`), so such tasks may only preempt their own
  CPU. Not tried yet: `scx_bpf_kick_cpu(SCX_KICK_PREEMPT)` and shrinking
  the victim's slice without an IPI (lavd).
- Victim rule (LATENCY mode): among the busy CPUs the woken task may use,
  the one whose current task has the largest vtime counting its run so far
  (from per-CPU `running_at`/`vtime`/`weight` written in running/stopping,
  read without locks), and only if larger than the woken task's queue
  vtime. With CPU-bound batch present, the victims are mostly batch tasks.
- Verified 2026-10-29 (manual 5 s loads): verifier accepts it; the BPF
  runnable count equals the kernel's `procs_running` in every one-second
  sample (idle: 1–3; with 4 zstd: 5–10); LATENCY mode with 4 zstd made
  583–4807 preemptions/s, BATCH mode none; unloads cleanly.
- Load signal normalization uses all 4 CPUs, although the workload runs on
  CPUs 0–2 and CPU 3 mostly holds the load generator.
- Batch classification (ed5d9ff, first piece of Feature A): per task, an
  EWMA (weight 1/8 per stop, `--class-shift 3`) of voluntary stops per
  stop, x1024, updated in `ops.stopping`. The kernel calls
  `stopping(p, false)` from `dequeue_task_scx` when the running task is
  dequeued (blocked or slept) and `stopping(p, true)` from
  `put_prev_task_scx` when it is put back still queued (slice expired or
  preempted) (`kernel/sched/ext.c`, v7.0). Batch while the EWMA < 512
  (`--batch-vol-max`); it starts at 1024 in `init_task`, so a task needs
  ~6 involuntary stops in a row to become batch. `running` copies the
  class into `cpu_ctx`; with `--victim batch`, `pick_victim` skips CPUs
  whose task is not batch (no victim: the woken task queues, as in BATCH
  mode). Stat `preempt_batch` counts preemptions of batch victims in
  either setting. Checked with 2 zstd + a 1 ms sleeper (2026-10-29):
  `--victim any` 84–98% of preemptions hit batch victims, `--victim
  batch` 100%. Each zstd pass is a new process and cannot be preempted
  until classified (~6 slices).
- Class changes are counted (`to_batch`, `from_batch` in the stats CSV)
  and, with `--trace-class`, printed with pid and comm (e1f2698).
- Classifier changed (3f3d11e): batch = has run `--batch-min-run-us`
  (default 20 ms) of CPU since it last slept, kept per task
  (`run_since_sleep`, reset by `stopping(p, false)`, grown by the run time
  on `stopping(p, true)`). `pick_victim` adds the victim's current run.
  Queue waiting doesn't count. Reason: the voluntary-stop EWMA marked busy
  server threads batch under overload (section 6.6). Go parks a locked
  worker thread (futex sleep) when sysmon preempts its goroutine after
  ~10 ms, so busy workers stay under 20 ms; zstd never sleeps. A worker
  in another language that never sleeps under a backlog would still turn
  batch; only a dependency signal would catch that. A "wakes others"
  signal (waker = `bpf_get_current_task_btf()` in `ops.runnable`, as
  scx_lavd does, skipping hardirq/softirq context) was considered and
  not used: when all Go Ps are held by busy workers, a finished request
  readies the handler goroutine on the worker's own P without any kernel
  wakeup. Each zstd pass is a new process, so zstd now shows one change
  to batch and one back (at exit) per pass.
- Correction (2026-10-30): zstd 1.5.7 runs 4 threads even with `-T1`
  (main, I/O, and two compression threads; `--single-thread` still has
  3). In one pass, sampled from `/proc/<pid>/task/*/sched` after 0.1 s:
  compression threads 152.8 ms CPU with 3 voluntary switches and 40.5 ms
  with 2; main thread 11.7 ms with 115; I/O thread 3.0 ms with 21. A whole
  pass makes ~600 voluntary switches. So zstd's heavy threads run ~50 ms
  bursts between short sleeps, and the light ones never reach 20 ms: under
  the run-time rule, zstd counts as batch only for part of each burst. The
  zstd "change back" events of section 6.6 are mostly these sleeps, not
  only exits.

---

## 5. Experiment log
One row per experiment run batch. Raw data lives in `results/<date>-<name>/`
together with `meta.json` (git commit, kernel, VM config, parameters).

| ID | Date | Commit | Config(s) | Workload / pattern | Reps | Data path | Status / one-line result |
|---|---|---|---|---|---|---|---|
| S1 | 2026-10-29 | 4edc764 + uncommitted phase 3 code | eevdf, kairos (scx_simple policy + enqueue printk) | `smoke.conf`: 5s@500, 10s@3000, 5s@500 req/s; 100 µs work; 4 zstd jobs | 1 | `results/2026-10-29-smoke/` | Harness works (latency + throughput logs). Smoke test, not a measurement |
| N1 | 2026-10-29 | 3d74753 | eevdf | `noise-batch.conf`: 10s@1000, 10s@3000; 100 µs; 4 zstd; server+batch CPUs 0–2, loadgen CPU 3 | 10 | `results/2026-10-29-noise-batch/` | Noise floor. p99 @3000 median 109 ms, range 77–1146 (section 6) |
| N2 | 2026-10-29 | 3d74753 | eevdf | `noise-nobatch.conf`: same, no batch | 10 | `results/2026-10-29-noise-nobatch/` | p99 @3000 median 12 ms, range 3.5–617 |
| N3 | 2026-10-29 | ce5a697 | eevdf | `noise-batch.conf` again, server GC logged | 10 | `results/2026-10-29-noise-batch-164528/` | p99 @3000 median 38 ms, range 11–1238; episodes not explained by GC |
| E1p | 2026-10-29 | bd50425 + uncommitted config | eevdf, never, always | `e1-pilot.conf`: 8 s each at 500/1000/2000/3000/4000 req/s; 500 µs work; 4 zstd | 1 | `results/2026-10-29-e1-pilot/` | Pilot. EEVDF run invalid (memory pressure from the VS Code C/C++ extension: kswapd 35%, kcompactd 20% CPU; calibration 868 instead of ~1000) |
| E1 | 2026-10-29 | 5ec281d | eevdf, never (20 ms slice), never3 (3 ms slice), always (3 ms) | `e1.conf`: as the pilot | 10 | `results/2026-10-29-e1/` | No latency crossover over 500–3000 req/s: always has the lowest p50 everywhere; p99 always ≈ never3; never (long slices) gives more batch throughput at 2000–3000 req/s (section 6.2) |
| E1bp | 2026-10-29 | 2d885f7 | eevdf, never, always | `e1b-pilot.conf` (no batch, GOMAXPROCS 3), `e1b-pilot-gmp16.conf` (no batch, GOMAXPROCS 16), `e1b-pilot-z2.conf` (2 zstd, GOMAXPROCS 3); 500 µs; 8 s steps 1000–6000 req/s | 1 | `results/2026-10-29-e1b-pilot{,-gmp16,-z2}/` | Pilot. Load generator keeps up to 6000 req/s; with GOMAXPROCS 3 the kernel sees ≤ ~1 runnable per CPU without batch; with 16 up to 4.7 (section 6.3). EEVDF-gmp16 and always-z2 runs disturbed (memory pressure; VS Code `node` 32%) |
| E1b | 2026-10-29 | 3a9b17e | eevdf, never, never3, always | `e1b.conf`: no batch, server GOMAXPROCS 16, 500 µs, 8 s steps 1000/2000/3000/3500/4000/4500/5000 req/s | 10 | `results/2026-10-29-e1b/` | No crossover: never3 ≥ always at every rate. always has the higher p50 in 10/10 reps at every rate (0.8 vs 0.7 ms at 1000, 4.1 vs 1.1 at 4000) because workers are preempted mid-request (section 6.4) |
| E1c | 2026-10-29 | 0b25c6b | eevdf, never, never3, always, batchvic (`--force-preempt --victim batch`) | `e1c.conf`: 2 zstd, server GOMAXPROCS 16, 500 µs, 8 s steps 1000/2000/3000/3500/4000/4500 req/s | 10 | `results/2026-10-29-e1c/` | batchvic has the lowest p50 at every rate (8–10/10 paired up to 4000) with workers' service time unchanged; more batch than always, less than never3. Plain always crosses never3 at ~4000 req/s. 2 batchvic reps collapsed at 4000–4500 (section 6.5) |
| DC1 | 2026-10-30 | e1f2698 | batchvic + `--trace-class` (EWMA classifier) | `diag-class.conf`: 2 zstd, GOMAXPROCS 16, 8 s at 3000/4500/5500 req/s | 2 | `results/2026-10-30-diag-class-012039/` | Overload diagnostic. 45 server threads relabelled batch 488 times (section 6.6). IDE in use |
| DC2 | 2026-10-30 | 3f3d11e | as DC1, run-time classifier | as DC1 | 2 | `results/2026-10-30-diag-class-012531/` | 28 server threads relabelled 42 times; one rep still slow from 4500 req/s (section 6.6). IDE in use |
| E1c2 | 2026-10-30 | 661c254 | never3, always, batchvic (run-time classifier) | `e1c2.conf`: as E1c | 10 | `results/2026-10-30-e1c2/` | No failed requests in any run; slow reps 2/3/3 (never3/always/batchvic). batchvic p50 below never3 in 8–10/10 up to 4000 req/s, below always in 7–9/10 at 3500–4500 but only 2–4/10 at 2000–3000; batch MB/s above always 8–10/10 (section 6.7) |

### 5.1 Planned experiments
| # | Experiment | Pattern | Shows |
|---|---|---|---|
| E1 | Steady-load sweep, always vs never preempt | constant rates | crossover; calibrates LOW/HIGH |
| E2 | Main comparison vs all baselines | synthetic periodic + Azure 2021 replay | per-phase benefit |
| E3 | Burst-length sweep | 10 ms – 10 s bursts | burst length vs reaction time |
| E4 | Transitions | sharp starts/ends | spikes at switches; fast exit value |
| E5 | Feature A ablation | E2 patterns | value of classification |
| E6 | Starvation stress | many LC-looking threads + batch | B bounds longest wait |
| E7 | Overhead | light load | cost vs scx_simple |
| E8 | ML burst prediction | periodic and random | ML vs persistence vs reactive |
| E9 | ML classifier (optional) | held-out workloads | learned vs hand-written tree |
| G | Generality | Redis instead of gRPC | conclusions hold |

**Configurations:** EEVDF, scx_bpfland, scx_lavd, ours forced always-preempt,
ours forced never-preempt, ours adaptive (± A, B, ML).

**Workloads:** gRPC server in Go (worker pool, configurable CPU work per request;
primary) and Redis ×N (secondary); zstd compression (primary batch), stress-ng
(controlled stress). Open-loop Go load generator with Poisson arrivals, phase
schedules or Azure Functions 2021 trace replay (time-compressed, rate-scaled).

**Metrics:** p50/p99/p99.9 per phase (quiet, burst, transition); achieved rate;
batch MB/s; preemptions, context switches, mode switches, time per mode, longest
wait; runqlat.

**Method:** ≥ 5 reps; alternate configurations; cooldowns; plugged in; Low Power
Mode off; identical VM; report mean and spread; oversubscribe CPUs.

---

## 6. Results
(For each experiment: the figure/table, the numbers, and the data path.
Keep interpretation in section 7.)

### 6.1 Noise floor (N1–N3, EEVDF, 2026-10-29)
Median across 10 reps [min–max] and CV (stdev/mean) of the per-run values
(`analysis/aggregate.py`).

| Run | Phase | p50 ms | p99 ms | p99.9 ms | send lag p99 ms | batch MB/s |
|---|---|---|---|---|---|---|
| N1 (4 zstd) | 1000 req/s | 1.00 [0.86–1.34] 16% | 7.8 [5.0–26.6] 65% | 24.9 [9.3–61] 68% | 2.7 [1.1–10.7] | 755 [574–780] 9% |
| N1 | 3000 req/s | 1.39 [1.22–18.7] 167% | 109 [77–1146] 131% | 178 [117–1525] 119% | 4.9 [2.4–289] | 659 [463–697] 12% |
| N3 (4 zstd, repeat) | 1000 req/s | 1.13 [0.96–1.30] 11% | 12.9 [5.9–85.5] 110% | 39.1 [10.4–119] 79% | 3.2 [1.7–18.3] | 704 [600–754] 7% |
| N3 | 3000 req/s | 1.27 [1.00–3.87] 54% | 37.9 [10.6–1238] 213% | 62.0 [24.0–1916] 214% | 3.8 [2.2–119] | 647 [402–689] 14% |
| N2 (no batch) | 1000 req/s | 0.53 [0.41–0.55] 8% | 9.1 [3.3–66.8] 106% | 66.7 [6.5–382] 123% | 2.9 [1.4–20.5] | — |
| N2 | 3000 req/s | 0.31 [0.30–0.43] 12% | 11.7 [3.5–617] 207% | 31.1 [6.3–738] 184% | 3.1 [1.6–192] | — |

Other per-run values (median [min–max]): context switches 56.7k/s
[49.0–59.9] (N1), 66.9k/s [57.1–68.7] (N2); load generator 29% of one CPU
(N1), 23% (N2); processes outside the experiment 13% [8.5–27] of one CPU (N1),
16% [8.4–45] (N2), 26% [10.5–38] (N3), mostly `node` (VS Code server) and
`claude` (~4–6% each), then kworkers.

Slow requests come in episodes. Per run, requests > 50 ms fall in 0–55 of
the 200 windows of 100 ms (by intended send time); in N1 runs with 4 zstd,
send lag during most episodes stays at 1–3 ms (the server side is slow, the
load generator isn't), while in N2 episodes the send lag rises too. N3,
server GC vs episodes (`analysis/episodes.py`, threshold 50 ms): ~65 GC
cycles per run during the load, wall time median 2–4 ms; ~33% of all 100 ms
windows overlap a GC cycle. Of the 30 episodes in N3, several overlap no GC
(e.g. r2, r6, 4 in r9), and GC cycles inside episodes mostly take their
usual 2–7 ms. Only the largest episode (r8, 3.5 s, latency up to 2 s) has
long GC cycles (up to 98 ms).

### 6.2 E1: steady-load sweep (2026-10-29)
`results/2026-10-29-e1/`, commit 5ec281d, all 40 runs clean. 4 zstd jobs and
the server on CPUs 0–2, load generator on CPU 3; 16 workers, 500 µs work
per request (calibration 994 iterations/µs); 8 s per rate, rising. Configs:
EEVDF; never = `--never-preempt` (20 ms slice); never3 = `--never-preempt
--batch-slice-us=3000`; always = `--force-preempt` (3 ms slice). Medians over
10 reps [range in `analysis/compare.py` output]; failed requests (5 s
timeouts) count as infinitely slow ("inf"). Load = scheduler's slow EWMA of
runnable tasks per CPU (4 CPUs).

| req/s | metric | eevdf | never | never3 | always |
|---|---|---|---|---|---|
| 500 | p50 / p99 ms | 2.3 / 55 | 5.1 / 81 | 2.2 / 48 | 1.2 / 23 |
| | batch MB/s | 483 | 525 | 473 | 458 |
| | load, preempt/s | – | 2.31, 0 | 2.15, 0 | 1.48, 17.0k |
| 1000 | p50 / p99 ms | 2.9 / 290 | 6.4 / 297 | 2.1 / 31 | 1.1 / 49 |
| | batch MB/s | 389 | 422 | 437 | 423 |
| | load, preempt/s | – | 2.51, 0 | 2.27, 0 | 1.75, 23.9k |
| 2000 | p50 / p99 ms | 1367 / 3895 | 7.0 / 215 | 2.6 / 97 | 1.2 / 163 |
| | batch MB/s | 312 | 338 | 283 | 262 |
| | load, preempt/s | – | 2.41, 0 | 2.33, 0 | 2.10, 26.6k |
| 3000 | p50 / p99 ms | inf / inf | 12.2 / 2249 | 4.3 / 444 | 1.9 / 226 |
| | batch MB/s | 268 | 218 | 174 | 196 |
| | load, preempt/s | – | 2.34, 0 | 2.30, 0 | 2.17, 23.4k |
| 4000 | p50 / p99 ms | inf / inf | 2766 / inf | 1833 / inf | 1357 / 3924 |

Paired counts (same repetition; configurations interleaved), at 500 /
1000 / 2000 / 3000 req/s:
- p50 lower for always than never3: 9, 9, 10, 8 of 10; than never: 9, 10,
  10, 8; than EEVDF: 9, 9, 10, 10.
- p99 lower for always than never3: 6, 6, 5, 6 of 10 (no difference); than
  never: 8, 7, 6, 6.
- Batch MB/s higher for never than always: 6, 6, 9, 7 of 10; never than
  never3: 5, 5, 8, 8.
- EEVDF fails at ≥ 2000 req/s: p50 1.4 s at 2000, every run has failed
  requests from 3000 on (median 21.6k failed at 3000); the scx_kairos modes
  sustain 3000.
Whole run (median): context switches 49.5k/s always, 16.4k never, 18.9k
never3, 27.4k EEVDF. Other processes used 23–26% of one CPU per run
(median; up to 94% in one never3 run).
Scheduler signals from 500 to 3000 req/s (median over reps, computed from
`sched_stats.csv` with runlib): slow load 2.31→2.34 never (max 2.51),
2.15→2.30 never3, 1.48→2.17 always; wakeups/s (all tasks) 8.8k→16.8k
never, 12.1k→20.2k never3, 22.8k→38.2k always.

### 6.3 E1b pilot: request threads competing (2026-10-29)
One run per configuration, so tails are anecdotes (section 6.1); only the
loads, the load generator's capacity and large effects count. 500 µs per
request, 8 s per rate, server on CPUs 0–2, load generator on CPU 3.
Load = slow EWMA of runnable tasks per CPU (4 CPUs).
- Load generator: 26–61% of CPU 3; send lag p99 ≤ 17 ms whenever the
  server keeps up, up to 6000 req/s. It reaches seconds only once the
  server is overloaded (guess: HTTP/2 flow control stops sends while the
  server doesn't read).
- No batch, GOMAXPROCS 3: load 0.4 → 1.0 (never) / 0.4 → 1.1 (always)
  from 1000 to 4000 req/s. Every configuration is overloaded at 5000
  (p50 > 0.7 s). At 4000: never p50/p99 0.8/32 ms, always 29/1159 ms,
  EEVDF 1307/3461 ms (one run each).
- No batch, GOMAXPROCS 16: load at 1000/2000/3000/4000/5000 req/s
  0.5/0.7/1.3/4.3/4.1 (never), 0.4/0.8/2.2/2.7/4.2 (always). never and always sustain 5000 without errors
  (p50 4.2 / 6.0 ms), all are overloaded at 6000. p99 never vs always:
  3 vs 5 ms at 2000, 24 vs 564 at 3000, 2048 vs 182 at 4000, 373 vs 99 at
  5000. GC ran about twice as often (cycle 50 at 11–12 s vs 20–21 s with
  GOMAXPROCS 3); heap goal 4 MB in both. The EEVDF run is disturbed
  (kswapd + kcompactd 17%, other processes 49% of a CPU) and failed from
  4000.
- 2 zstd, GOMAXPROCS 3: batch 637 (never) vs 309 MB/s (always) at
  1000 req/s; zstd passes took 0.24 s vs 0.44 s (median). always had the
  lower p50 at 2000–4000 req/s (1.0 vs 2.3, 1.2 vs 4.4, 3.6 vs 10.4 ms);
  p99 mixed. VS Code's `node` used 32% of a CPU during the always run.

### 6.4 E1b: request threads competing, no batch (2026-10-29)
`results/2026-10-29-e1b/`, commit 3a9b17e, all 40 runs clean (other
processes 5–7% of a CPU, median). No batch jobs; server GOMAXPROCS 16,
16 workers, 500 µs per request, server on CPUs 0–2, load generator on
CPU 3. Configurations as in E1. Medians over 10 reps; ms.

| req/s | eevdf p50/p99 | never | never3 | always | load never3 / always | always preempt/s |
|---|---|---|---|---|---|---|
| 1000 | 0.8 / 3 | 0.7 / 8 | 0.7 / 2 | 0.8 / 5 | 0.40 / 0.44 | 6.7k |
| 2000 | 0.8 / 22 | 0.7 / 18 | 0.7 / 13 | 0.8 / 6 | 0.73 / 0.80 | 18.0k |
| 3000 | 1.0 / 31 | 0.8 / 34 | 0.8 / 23 | 1.1 / 47 | 1.23 / 1.71 | 32.8k |
| 3500 | 1.3 / 14 | 0.9 / 53 | 0.9 / 8 | 2.4 / 81 | 1.49 / 2.52 | 42.7k |
| 4000 | 1.7 / 33 | 1.1 / 60 | 1.1 / 39 | 4.1 / 58 | 1.97 / 3.44 | 47.1k |
| 4500 | 2.2 / 19 | 1.7 / 92 | 1.8 / 172 | 5.0 / 32 | 2.65 / 3.85 | 40.5k |
| 5000 | 4.2 / 129 | 4.1 / 282 | 2.6 / 70 | 12.7 / 257 | 3.16 / 4.20 | 25.0k |

Paired counts at 1000 / 2000 / 3000 / 3500 / 4000 / 4500 / 5000 req/s:
- p50 lower for always than never3: 0 of 10 at every rate; than never:
  0 at every rate except 3 at 5000.
- p99 lower for never3 than always: 5, 5, 6, 9, 7, 5, 7 of 10; never3 than
  never: 7, 7, 7, 8, 5, 5, 8; never3 than EEVDF: 6, 5, 7, 9, 5, 4, 6.
- p50 lower for never3 than EEVDF: 10 of 10 up to 4000, then 6 and 8.
No failed requests in any run. Server-side medians (from the reply's
queue and service times): queue time equal for never3 and always
(≤ 0.04 ms up to 4000 req/s); service time (wall time of the 500 µs spin)
never3 0.52–0.53 ms at every rate, always 0.56, 0.57, 0.75, 1.57, 2.58,
2.57, 1.76 ms. Load generator send lag p99 (median): ≤ 2.4 ms for eevdf,
never3 and always except never3 6.4 ms at 4500; never 4–26 ms from 2000.
Whole run: context switches 93.8k/s always, 57–59k never/never3, 67.8k
EEVDF; system time 12.6% always, 7.6–7.8% never/never3, 9.2% EEVDF.

E1 rechecked the same way: server-side p50 (queue + service) at 500 req/s
is 0.64–0.68 ms for never, never3 and always, while end-to-end p50 is
5.1, 2.2 and 1.2 ms; median send lag ≤ 0.11 ms. Send lag p99 in E1 was
24–60 ms for never, 6–32 ms for never3 and 6–8 ms for always at
500–3000 req/s.

### 6.5 E1c: batch and request threads mixed; batch-only victims (2026-10-29)
`results/2026-10-29-e1c/`, commit 0b25c6b, all 50 runs clean (other
processes 9–11% of a CPU, median). 2 zstd jobs and the server on CPUs
0–2, server GOMAXPROCS 16, 16 workers, 500 µs, load generator on CPU 3.
batchvic = `--force-preempt --victim batch` (3 ms slice). Medians over
10 reps.

| req/s | p50 ms: eevdf / never / never3 / always / batchvic | p99 ms: same order | batch MB/s: never3 / always / batchvic | preempt/s: always / batchvic |
|---|---|---|---|---|
| 1000 | 1.2 / 1.3 / 1.2 / 0.9 / 0.8 | 9 / 27 / 8 / 22 / 11 | 618 / 500 / 539 | 23.4k / 7.8k |
| 2000 | 1.9 / 4.1 / 1.9 / 0.9 / 0.9 | 104 / 41 / 41 / 54 / 68 | 469 / 346 / 383 | 35.0k / 11.9k |
| 3000 | 4.0 / 6.7 / 3.4 / 1.2 / 0.9 | 163 / 218 / 28 / 50 / 177 | 350 / 222 / 257 | 43.0k / 11.9k |
| 3500 | 3.5 / 6.4 / 3.5 / 1.8 / 1.0 | 85 / 141 / 72 / 30 / 15 | 285 / 149 / 218 | 46.7k / 10.1k |
| 4000 | 6.2 / 10.9 / 3.7 / 4.3 / 1.1 | 396 / 191 / 33 / 67 / 29 | 203 / 87 / 149 | 44.7k / 7.6k |
| 4500 | 8.1 / 42.6 / 8.8 / 11.0 / 4.8 | 318 / 679 / 468 / 271 / 684 | 124 / 90 / 92 | 26.4k / 4.2k |

Paired counts at 1000 / 2000 / 3000 / 3500 / 4000 / 4500 req/s (of 10):
- p50 lower for batchvic than always: 8, 9, 8, 10, 8, 7; than never3:
  10, 10, 10, 10, 8, 5; than EEVDF: 10, 10, 10, 10, 8, 8.
- p50 lower for always than never3: 10, 10, 10, 10, 5, 4.
- p99 lower for batchvic than never3: 5, 5, 7, 7, 7, 5; than always: 3,
  4, 4, 7, 6, 2; than EEVDF: 6, 5, 7, 7, 8, 4.
- Batch MB/s higher for batchvic than always: 8, 9, 9, 9, 8, 6; for
  never3 than batchvic: 10, 10, 10, 9, 10, 8; for never than never3: 6,
  7, 7, 7, 9, 10.
Service time (wall time of the 500 µs spin) p50: never3 and batchvic
0.50–0.51 ms at every rate; always 0.51, 0.54, 0.64, 1.08, 2.03, 1.57 ms.
Under always, 70–79% of preemptions hit a batch-classified task.
Whole run: context switches 88.6k/s always, 53.7k batchvic, 55.8k EEVDF,
34.7k never3, 30.4k never; system time 15.3%, 12.0%, 8.8%, 7.3%, 7.5%.
Slow runs (some phase with p50 > 100 ms): eevdf 2, never 1, never3 3,
always 3, batchvic 2 of 10 each, all at 4000–4500 req/s. Only batchvic's
two had failed requests: r6 (p50 678 ms at 4000, 3.4 s at 4500, 198
failed) and r7 (2.9 s at 4000, 28.5k failed at 4500). In both, runnable
tasks jumped from ~10 to ~28–30 within a second, wakeups/s fell from ~47k
to ~15k, and up to 90% of wakeups found a batch victim (r7), against
20–30% before. Other processes used 35% and 24% of a CPU in these two runs
(median for batchvic 11%).

### 6.6 Class changes under overload (DC1, DC2; 2026-10-30)
One-off diagnostics, 2 reps each, IDE in use (not for performance).
batchvic with `--trace-class`, 2 zstd, GOMAXPROCS 16, 8 s each at 3000,
4500, 5500 req/s. Class changes from `trace_pipe`; per-second counts
from `sched_stats.csv`. (The first DC1 attempt captured no trace:
`trace_pipe` reported busy once; rerun.)
- DC1, voluntary-stop EWMA classifier: 1209 class changes. `grpcserver`
  488 to batch / 459 back, 45 distinct threads; zstd 134 to batch. At
  3000 req/s 3–7 changes to batch per second (new zstd processes). In
  the rep that collapsed at 4500 req/s (p50 359 ms), 23–41/s; in the rep
  that held (p50 4.8 ms), 1–3/s. At 5500 (both overloaded), 4–44/s.
- DC2, run-time classifier: `grpcserver` 42 to batch / 42 back, 28
  distinct threads; zstd 758 / 758 (one change each way per pass, the
  second at exit); loadgen 85, claude 47, node 34. Changes to batch
  5–24/s in every phase. Rep 1 was still slow at 3000 req/s (p50
  3.2 ms, p99 190 ms) and collapsed from 4500 (p50 980 ms, 3770 failed
  at 5500); rep 2 p50 1.2 ms / 6.1 ms at 3000 / 4500.

### 6.7 E1c2: E1c with the run-time classifier (2026-10-30)
`results/2026-10-30-e1c2/`, commit 661c254, all 30 runs clean (other
processes 12–14% of a CPU, median). Workload and rates as E1c. Medians
over 10 reps; ms.

| req/s | p50 never3 / always / batchvic | p99 same order | batch MB/s same order | preempt/s always / batchvic |
|---|---|---|---|---|
| 1000 | 1.2 / 0.8 / 0.8 | 31 / 4 / 6 | 593 / 507 / 563 | 23.6k / 6.7k |
| 2000 | 1.9 / 0.9 / 0.9 | 20 / 95 / 75 | 486 / 337 / 410 | 35.4k / 8.3k |
| 3000 | 3.0 / 1.1 / 1.2 | 202 / 113 / 45 | 335 / 229 / 295 | 43.4k / 7.4k |
| 3500 | 2.8 / 1.8 / 1.4 | 120 / 146 / 60 | 282 / 147 / 223 | 44.1k / 5.6k |
| 4000 | 3.2 / 5.2 / 2.1 | 419 / 135 / 344 | 185 / 98 / 162 | 42.8k / 4.3k |
| 4500 | 4.2 / 37.3 / 12.0 | 487 / 277 / 601 | 124 / 91 / 103 | 20.5k / 2.2k |

Paired counts at 1000 / 2000 / 3000 / 3500 / 4000 / 4500 req/s (of 10):
- p50 lower for batchvic than never3: 10, 10, 9, 9, 8, 4; than always:
  7, 2, 4, 7, 9, 7; for always than never3: 10, 10, 9, 9, 5, 3.
- p99 lower for batchvic than never3: 9, 4, 8, 6, 5, 5; than always: 2,
  6, 3, 5, 3, 4.
- Batch MB/s higher for batchvic than always: 9, 8, 9, 9, 9, 10; for
  never3 than batchvic: 7, 9, 10, 10, 9, 8.
No failed requests in any run. Slow runs (some phase p50 > 100 ms):
never3 2 (from 4500 req/s), always 3 (one from 3000), batchvic 3 (one
from 3500); these runs had 12–31% other-process CPU. Service time p50:
never3 and batchvic 0.51 ms at every rate; always 0.52 → 1.49 ms at 4000.
Changes to batch per second (median) 43 at 1000 req/s falling to 7–9 at
4500 in both always and batchvic runs; under always, 3–20% of
preemptions hit a batch-classified task (E1c, EWMA classifier: 70–79%).

---

## 7. Explanations and insights
(The "why" behind each result. This is what the report is graded on.)

Expected explanations to test, not yet confirmed:
- Preemption pays off at low load (no queue, immediate service) and stops paying
  off at high load (switch cost + cache pollution + broken batching × queue length).
- Adaptivity helps only when bursts last longer than the scheduler's reaction time.
- Persistence is a strong burst predictor because bursts persist; ML should help
  mainly for periodic bursts.

Noise floor (N1–N3):
- Per-run p99 and p99.9 are decided by a few slow episodes (hundreds of ms
  to seconds), so they vary 3–50× between reps of the same config, while p50
  and batch throughput vary ~10%. The server's GC is not the main trigger:
  episodes occur without GC, at about the rate GC overlaps any window, and
  GC is only long inside the worst episode (guess: GC slowed by the episode,
  not causing it). Cause still unknown. Candidates: the host not running
  vCPUs 0–2 (section 8: 55–81 ms per 10 s per vCPU unexplained), bursts of
  other guest processes (VS Code server, Claude Code), or a backlog that
  builds after a short stall and drains slowly when 4 zstd jobs share the
  CPUs. Without batch (N2) the load generator stalls in the same episodes,
  which points at the whole VM (guess).
- The medians of 10 reps also moved between batches of the same config
  (p99 @3000 req/s: 109 ms in N1, 38 ms in N3, an hour apart). So compare
  configurations only within one batch, interleaved (run.sh alternates the
  order), never across batches.

E1 (section 6.2):
- No latency crossover in the tested range (load 1.5–2.6 runnable per CPU,
  service using 0.25–1.5 of 3 CPUs): always-preempt never had worse latency
  than never-preempt. The hypothesis predicted that never-preempt would win
  at high load; here it only wins on batch throughput, and only with its
  long slice (never vs always at 2000 req/s: 338 vs 262 MB/s, 9/10 reps).
- The median gain is from preemption itself: always vs never3 have the same
  slice, and always's p50 is 1.1–1.9 ms vs 2.1–4.3 ms. Guess: without
  preemption a woken worker waits for a zstd slice to end, up to 3 ms, at
  each of several wakeups per request (handler, worker, reply).
- The tail gain over never is mostly from the shorter slice: p99 of always
  and never3 don't differ (5–6 of 10), while never (20 ms) is worse. Guess:
  tails are set by the episodes of section 6.1 and by queueing that
  preemption does not remove.
- Why no crossover (guess): the victim rule only preempts a task ahead in
  vtime, which is almost always a zstd job, so latency tasks rarely preempt
  each other in this range; the costs the hypothesis names (preempting
  latency work, broken batching of the service) would need the CPUs full of
  service threads. At 4000 req/s every config is overloaded.
- The measured load depends on the mode: at the same offered load, always
  reads lower (1.48 at 500 req/s) than never (2.31), since requests finish
  sooner and fewer tasks are queued. An adaptive switch on this signal sees
  load drop when it enters LATENCY mode, which hysteresis has to allow for.
- With 4 zstd jobs keeping the CPUs full, the runnable count hardly moves
  with the request rate under never (2.3–2.5 from 500 to 3000 req/s): the
  batch jobs dominate it, so a switch on it cannot tell light from heavy
  service load. The wakeup rate does rise with the request rate in every
  mode, but is 2–2.6× higher under always at the same rate (guess: Go's
  threads park and wake more often when they get a CPU at once). The
  phase 5 switch needs a signal that tracks service demand in both modes;
  candidates: wakeup rate with per-mode thresholds, or the wait of woken
  tasks only.
- EEVDF collapses at 2000 req/s while all scx_kairos modes cope with 3000.
  Unexplained. Guess: the server has GOMAXPROCS=3 and 16 locked worker
  threads; a thread descheduled while holding one of Go's 3 Ps blocks Go
  work until it runs again, and EEVDF's placement of woken threads behind
  zstd makes that more likely than our 20 ms sleeper credit. Check with
  E2's EEVDF baseline and `perf sched`/runqlat before claiming it.

E1b pilot (section 6.3):
- The Go runtime is a second scheduler in front of ours. With GOMAXPROCS
  equal to the CPU count (3), at most 3 workers run Go code; the rest
  sleep inside the runtime waiting for a P, invisible to the kernel. So
  without batch the kernel never holds a queue of request threads, the
  wakeup-preemption choice has little to act on, and overload shows up as
  Go-level queueing (all configurations fail at 5000 req/s). This also
  explains part of E1's missing crossover: latency threads could not
  oversubscribe the CPUs among themselves (guess for E1; measured here).
- GOMAXPROCS 16 makes the workers' competition visible (load up to 4.7)
  and raised the sustainable rate from ~4000 to 5000 req/s. It is a
  realistic setting: Go before 1.25 ignored container CPU limits, and
  thread-pool servers in other languages expose every thread to the
  kernel. Whether never or always wins there is not settled by one run
  each (opposite winners at 3000 and 4000–5000 req/s).
- With 2 zstd, always-preempt halved zstd's throughput at 1000 req/s
  (20k preemptions/s against 2 jobs). Guess: with one CPU left for the
  service, every wakeup that finds the service's CPU busy preempts a zstd
  job, i.e. every ~100 µs per job.

E1b (section 6.4):
- Preempting request threads hurts, and the harm grows with load. With
  only the server's threads competing, the woken task's victim (largest
  vtime) is a worker in the middle of a request: always's service time
  for 500 µs of work rises from 0.56 to 2.6 ms between 1000 and 4500
  req/s, while never3's stays 0.53 (measured). Its p50 is higher in every
  rep at every rate, and its p99 is worse than never3's at 3500, 4000 and
  5000 req/s. This is the cost half of the hypothesis.
- Still no crossover: at low load, preemption does not help when the only
  possible victims are request threads (p99 ties, p50 0.05 ms worse at
  1000 req/s). The benefit half of the hypothesis appeared only in E1,
  where the victims were zstd jobs.
- Together, E1 and E1b say the better fixed policy depends on who gets
  preempted, not on the load level: preempting batch helps latency (E1),
  preempting request threads hurts it (E1b). The current victim rule
  can't tell them apart. Class-aware preemption (Feature A, section 3.5)
  is therefore the main lever; a load switch alone would have to guess
  the victim's class from the load.
- always preempts 7–12 times per request (6.7k/s at 1000 req/s, 42.7k/s at
  3500). Guess: most wakees are Go runtime threads (netpoller, spinning Ms
  looking for work with 16 Ps), not request work, so many preemptions buy
  nothing. A per-victim and per-wakee breakdown would check this.
- E1 refined: there the server-side time was the same in all modes, so
  always's lower p50 in E1 came from the request's path outside the
  worker (gRPC handler and reply threads waiting behind zstd), not from
  the worker itself.
- The measured load again depends on the mode, now in the other
  direction: always reads higher (3.44 vs 1.97 at 4000 req/s), since
  preempted workers stay runnable longer.
- Instrument caveat: the load generator is scheduled by the scheduler
  under test (section 8). Under never (20 ms slices) its send lag p99 is
  4–26 ms in E1b and 24–60 ms in E1, so never's tails include the
  instrument's delay. Comparisons between always and never3 (both 3 ms)
  are not affected this way in E1b, and its p50 result runs against the
  lag bias (always has the lower lag).
- EEVDF does not collapse in E1b; E1's collapse needs GOMAXPROCS 3 and/or
  the zstd jobs (open, section 9).

E1c (section 6.5):
- Plain always-preempt now shows a load crossover against never3 in p50:
  better in 10/10 reps up to 3500 req/s, even at 4000 (5/10), worse at
  4500 (4/10). It appears where workers start being preempted mid-request
  (service time 0.64 → 2.03 ms from 3000 to 4000 req/s). So the
  hypothesis's crossover exists when batch and request threads are mixed,
  with the current victim rule.
- Preempting only batch-classified tasks removes it: batchvic has the
  lowest p50 at every rate (8–10 of 10 against every other config up to
  4000 req/s), workers' service time stays at 0.50–0.51 ms, and it makes
  3–6× fewer preemptions than always. It keeps more batch throughput than
  always (8–9 of 10) but 13–27% less than never3 (median, 1000–4000
  req/s), the price of preempting zstd. p99 differences are mostly within
  the noise (batchvic below never3 in 7/10 at 3000–4000).
- So "preempt batch, protect request threads" is the better policy for
  latency at every load here; what remains is a trade-off between batch
  throughput and latency, which a load-based switch can manage (e.g.
  BATCH mode while latency has headroom).
- Failure under overload (likely, inferred from the stats, not measured
  per task): when a backlog builds, workers take job after job without
  sleeping, use whole slices, and the classifier marks them batch after
  ~6 slices. batchvic then preempts workers like always does, which
  slows them further; r7's 90% of wakeups finding a batch victim while
  ~28 tasks were runnable and only 2 were zstd fits this. The trigger is
  unknown (both runs had more other-process CPU than usual). Needs a
  class-transition counter to confirm, and a classification that does
  not flip under backlog (slower EWMA, or Feature A's dependency features:
  woken by other tasks, wakes others), plus Feature B's max-wait guard.
- The measured load reads lower under batchvic (1.20–2.57) than under
  never3 (1.50–3.86) at the same rate, as with always in E1: the signal
  stays mode- and policy-dependent.

DC1/DC2 (section 6.6):
- Confirmed: under overload, the EWMA classifier relabelled busy server
  threads as batch, many times a second, in the rep that collapsed.
- The run-time classifier removes most of that (42 vs 488 relabellings),
  but a collapse at 4500 req/s still happened in 1 of 2 reps. So the
  relabelling was at most an aggravating factor; 4500 req/s with 2 zstd
  is near capacity, where every config had slow reps in E1c. Whether
  batchvic now fails no more often than the fixed modes needs E1c again.

E1c2 (section 6.7):
- With the run-time classifier, batchvic had no failed requests and no
  more slow runs than never3 or always (3 vs 2 and 3), against 2 of 10
  failing in E1c. One batch each, so this is suggestive, not proof.
- It still protects request threads (service time 0.51 ms at every rate)
  and has the lowest p50 from 3500 to 4000 req/s, but it preempts zstd
  about half as often as E1c's batchvic (5.6k vs 10.1k/s at 3500; cross-
  batch numbers, so rough), keeps more batch throughput than always at
  every rate, and loses its p50 edge over always at 2000–3000 req/s.
  Reason (measured in section 4): zstd's compression threads sleep every
  ~50 ms, so they are batch only after 20 ms into each burst; its main
  and I/O threads never are.
- E1c's "70–79% of always's victims are batch" was inflated: the EWMA
  counted the preemptions themselves as evidence, so the workers always
  preempted looked batch. Under the run-time rule, 3–20%.
- A burst-length average (Feature A's avg_burst_us: batch if the average
  CPU burst between sleeps, or the current one, is ≥ 20 ms) would keep
  zstd's compression threads batch right after their short sleeps while
  workers (0.5 ms bursts, ≤ ~10 ms under a backlog) stay out.

---

## 8. Limitations
- VM: vCPUs are macOS threads on mixed P/E cores; no SMT/NUMA topology; fanless
  thermal throttling.
- Synthetic service (gRPC CPU loop) may not reflect real request costs; mitigated
  with Redis runs.
- Trace replay is time-compressed and rate-scaled.
- VM noise (measured 2026-10-29; quick checks, raw output not kept): a thread
  pinned to each vCPU that only reads the clock lost 2–5% of 10 s to gaps
  > 100 µs (largest gap 6–27 ms). Most of that was waiting in the guest's run
  queue (other guest tasks such as the VS Code server and Claude Code, ~3% of
  a CPU); 55–81 ms per 10 s per vCPU is not explained by the guest (likely
  the host not running the vCPU; the hypervisor reports no steal time).
- Waking an idle vCPU is slow: on the idle VM, median request latency was
  ~2× higher at 500 req/s than at 2000 req/s (600–770 µs vs 320–350 µs).
  Low-load latencies include this host effect.
- The load generator runs in the same VM, so the scheduler under test also
  schedules it. In the S1 burst, its send lag p99 was 12 ms (EEVDF) and
  175 ms (scx_kairos): part of the measured latency is the instrument's own
  delay. From N1 on, the load generator has its own CPU (section 4); its send
  lag p99 is then 2–5 ms (median over reps) under EEVDF, still not zero.
- Run-to-run noise (section 6.1): tail percentiles of single runs can't be
  compared; p99 CV across reps is 65–210%. Comparisons need ≥ 10 reps
  interleaved in one batch, with medians and ranges reported.
- Processes outside the experiment used 8–45% of one CPU per run (VS Code
  server, Claude Code, kworkers). Final runs should be made with the IDE
  disconnected and Claude Code idle.
- Client on CPU 3 stands in for remote clients (as on a real deployment,
  where clients do not share the server's CPUs). Requests travel over
  loopback, not a NIC: loopback hands each packet to the sending CPU's
  receive backlog (`loopback_xmit` → `__netif_rx`, RPS off on `lo`), so
  the request's receive path, including the wakeup of the server thread,
  runs on CPU 3, while the reply's runs on the server's CPUs. With a NIC,
  the request's interrupt and receive work would land on the server's
  CPUs. Small next to 500 µs of work per request, but not zero.
- 3 CPUs is far fewer than a production server; with more CPUs an idle
  one is more often free, so preemption decisions matter less often.

---

## 9. Open questions
- Exact LOW/HIGH crossover on this VM? (E1)
  → E1 (2026-10-29) found no latency crossover between 500 and 3000 req/s
  with 4 zstd jobs. Open: does one appear (a) at lower load (0–2 batch
  jobs), (b) when the CPUs are full of latency-sensitive threads (no batch,
  more workers or shorter requests), or (c) only as a trade-off, i.e.
  never-preempt's batch gain while p99 stays under a target?
  → E1b pilot (section 6.3): (b) needs GOMAXPROCS above the CPU count,
  else Go hides the queue from the kernel.
  → E1b (section 6.4): no crossover either; never3 ≥ always at every
  rate, and preempting request threads costs more as load rises. Open:
  with batch and request threads mixed (2 zstd, GOMAXPROCS 16), does
  plain always-preempt show a load crossover, and does preempting only
  batch-class tasks beat both fixed modes at every load?
  → E1c (section 6.5): yes to both for p50 (always crosses never3 at
  ~4000 req/s; batchvic is best at every rate), at a batch-throughput
  cost vs never3. Open: why 2 of 10 batchvic runs collapsed at 4000–4500
  (classifier flipping workers to batch under backlog?), and a
  classification that stays correct under overload.
- Why does EEVDF collapse at 2000 req/s in E1 while our modes don't?
- Which preemption mechanism is most reliable?
- Can wakers be identified reliably, including interrupt-driven wakeups?
- Does ramping slices during a switch reduce transition spikes vs a hard switch?
- Does ML beat persistence for any pattern besides periodic?
- (Check once the scheduler is implemented, E7.) How long does the idle-CPU
  search in `select_cpu` take per call (`kernel.bpf_stats_enabled=1`,
  `bpftool prog show`: run_time_ns / run_cnt)? Does skipping it when load is
  known to be high save anything measurable, or is the shared DSQ's lock the
  bigger cost at high load?
- (Decide before E1.) How to keep the load generator out of the scheduler's
  way: its own CPU (taskset; server and batch on the other 3), real-time
  priority (chrt; needs root), or run it on the Mac host?
  → Decided 2026-10-29: its own CPU (section 4, section 10).
- What is the noise floor? Repeat EEVDF runs (with and without batch) to see
  the run-to-run spread before comparing schedulers. In S1, EEVDF's
  per-second p99 ranged 4–295 ms within one run.
  → Measured (N1–N3, section 6.1): large. Open: what causes the slow
  episodes? Next checks: a per-vCPU hiccup monitor during a run (host
  stalls), and whether episodes coincide with bursts of `node`/`claude`.
- S1 (one rep, printk on): scx_kairos (scx_simple policy) had half EEVDF's
  context switches (20.7k/s vs 40.6k/s) but more system time (24.8% vs
  18.5%) and lower batch throughput in the burst (231 vs 433 MB/s). Printk
  cost, the shared DSQ's lock, or tasks moving between CPUs? Recheck without
  the printk.

---

## 10. Session entries
Newest at the bottom. Template:

```
### YYYY-MM-DD — <short title>
**Did:** what was built, changed or run (with commit hashes)
**Found:** measured facts (numbers + data paths)
**Interpretation:** what we think it means (mark guesses)
**Decisions:** what was decided and why
**Problems:** errors, verifier issues, surprises, and how they were resolved
**Next:** concrete next steps
```

### 2026-10-26 — Design phase complete
**Did:** Studied course material; surveyed sched_ext schedulers (bpfland, flash,
lavd), EEVDF preemption features, preemption-cost research, task-criticality
research, traffic predictability, load generators and traces.
**Found:** Load-adaptive timeslices already exist in sched_ext schedulers (all
shrink slices as load rises). Wakeup preemption strategies differ widely (EEVDF:
deadline-based; lavd: top ~1.5% only via victim slice shrink; bpfland/flash: no
wakeup preemption). Linux's attempts to protect running tasks from wakeup
preemption caused starvation (2024, Jul 2026).
**Decisions:** Core = load-adaptive wakeup preemption + A (dependency-based
classification) + B (budget + max wait). ML in user space: burst prediction
primary, classification secondary, both compared with non-ML baselines. Services:
gRPC (primary), Redis (secondary). Batch: zstd (primary), stress-ng (stress).
Patterns: synthetic + Azure Functions 2021 trace. Baselines: EEVDF, bpfland, lavd,
own static modes.
**Next:** Phase 0 setup (VM, verify sched_ext, build/run scx_simple, bpfland, lavd);
draft proposal for Oct 30.

### 2026-10-26 — Environment and tooling plan
**Did:** Wrote setup instructions for the VM and for Claude Code; created
CLAUDE.md, this log, `scripts/log_entry.sh` and `scripts/meta.sh` (both tested).
**Found:** Ubuntu 26.04 LTS ships Linux 7.0 (sched_ext mainline since 6.12). scx
repo supports Ubuntu ≥ 25.04; Rust schedulers via cargo; scx_simple and other C
examples come from the kernel tree's `tools/sched_ext/`. Claude Code supports
Ubuntu on ARM64 with 4 GB+ RAM.
**Decisions:** Lima VM (4 vCPUs, 4 GB RAM, 40 GB disk) instead of UTM: scripted,
built-in SSH config, passwordless sudo. Repository lives inside the VM and is
pushed to a private GitHub repo. Claude Code installed inside the VM.
**Problems:** 4 GB RAM may be tight when compiling the Rust schedulers; plan to
add a swap file and build only the needed crates.
**Next:** Create the VM, verify `CONFIG_SCHED_CLASS_EXT`, build scx_simple,
scx_bpfland, scx_lavd; then the proposal (due Oct 30).

### 2026-10-27 — VM created; language decision
**Did:** Created the Lima VM, upgraded packages, installed build tools,
disabled the auto-started Redis service, added a 4 GB swap file.
**Found:** After reboot: Ubuntu 26.04.1 LTS, kernel 7.0.0-38-generic,
`CONFIG_SCHED_CLASS_EXT=y`, `/sys/kernel/sched_ext/state` = disabled (available,
not loaded). Kernel source tag to use for the examples: v7.0.
**Decisions:** Scheduler written entirely in C (BPF policy + loader), starting
from the kernel's `tools/sched_ext/scx_simple`. Rust only to build baselines
(bpfland, lavd), and only if no Ubuntu package provides them. Initial builds
happen inside `~/linux-src/tools/sched_ext` to reuse its Makefile; move to a
standalone build later.
**Problems:** Was working inside the read-only Mac mount; the repo must live in
the VM's own home.
`make -C ~/linux-src/tools/sched_ext -j4` failed while building bpftool. Fixed by
installing `libssl-dev libcap-dev binutils-dev llvm-dev`, running `make clean`,
and rebuilding without -j. Result: scx_simple, scx_qmap, scx_central,
scx_flatcg, scx_pair, scx_cpu0, scx_sdt, scx_userland in
`~/linux-src/tools/sched_ext/build/bin/`. (VM home is `/home/aayushnair.guest`.)
**Next:** Verify sched_ext; build and run scx_simple; obtain bpfland/lavd;
install Claude Code; first session.

### 2026-10-27 — scx_simple runs; baselines need building
**Did:** Loaded `scx_simple` from `~/linux-src/tools/sched_ext/build/bin/` and
unloaded it with Ctrl-C ("EXIT: unregistered from user space").
**Found:** On an idle VM, scx_simple's counters reached about local=4152,
global=446: roughly 10:1 tasks found an idle CPU in `select_cpu` (direct
dispatch to the local queue) rather than going through the global queue.
**Interpretation:** At low load almost every wakeup finds an idle CPU, so the
preempt-or-batch decision rarely comes into play. The local:global ratio is a
cheap indicator that an experiment has reached the contended regime (should be
recorded in our own stats map too).
**Decisions:** No Ubuntu package provides scx_bpfland/scx_lavd
(`apt-cache search scx` empty), so build them with cargo in `~/scx`.
**Next:** Build and test bpfland and lavd; install Claude Code; first session.

### 2026-10-27 — Baselines built and verified (phase 0 complete)
**Did:** Built scx_bpfland and scx_lavd (both 1.1.4, aarch64) with cargo in
`~/scx`; loaded and unloaded each cleanly.
**Found:**
- bpfland: 1 NUMA node, SMT off, primary CPU domain 0xf (all 4 CPUs).
- lavd: autopilot mode on; core compaction active (primary CPUs [0], overflow
  [1,2,3] at low load); slice_max 5000 µs, slice_min 500 µs; preempt_shift 6
  (about top 1.5% latency-critical tasks may preempt); no_preemption false.
- lavd printed "struct_ops member ... not found in kernel, skipping" for several
  newer fields: the scx tree is newer than kernel 7.0; fields are skipped
  harmlessly. Also "skeleton link is uninitialized" and "BPF stream dump
  unavailable" warnings, with no effect on loading.
**Interpretation:** lavd's power-saving core compaction can hurt its latency and
throughput in our experiments, which would make the comparison unfair.
**Decisions:** Evaluate lavd in its default (autopilot) mode and in its
performance mode (check exact flag with `scx_lavd --help`), report both. Record
the scx commit hash used for the baselines.
Baselines built from sched-ext/scx commit
`4c63759101a78d3595e8ec27c8c9c5aaac43b1fe`. Claude Code 2.1.311 installed in the
VM at `~/.local/bin/claude` (added to PATH in ~/.bashrc).
**Next:** Set up
`~/adaptive-sched`; first session (copy scx_simple as "adaptive", add bpf_printk).

### 2026-10-27 — Project named KairOS; repository connected
**Did:** Chose the name and created the GitHub repository
https://github.com/AayushNair10/KairOS. Set git identity (Aayush Nair,
aayushnair204@gmail.com) and default branch `main`.
**Decisions:** Project name **KairOS**, after Kairos, the Greek personification
of the opportune moment (as opposed to Chronos, clock time): the scheduler's job
is to preempt at the right moment rather than always or never. The name also
contains "OS". Scheduler program: `scx_kairos`. Imagery for the report/video:
Kairos holds a balance on a razor's edge (latency vs throughput) and has a
forelock but a bald back of the head (seize the opportunity as it arrives, i.e.
switch modes at the start of a burst).
**Next:** First commit and push; first Claude Code session (rename files to
KairOS/scx_kairos, record scx commit, copy scx_simple as scx_kairos, add
bpf_printk, build).

### 2026-10-28 — Repository on GitHub; plan file added
**Did:** Pushed the repository to https://github.com/AayushNair10/KairOS
(GitHub CLI login via device code), added `.claude/settings.json`, connected
VS Code via Remote-SSH to `lima-sched`. Added `docs/PROJECT_PLAN.md` (phases,
architecture, components, deliverables) so Claude Code sessions have the full
plan; CLAUDE.md now points to it.
**Next:** First Claude Code session (phase 2: scx_kairos skeleton with
bpf_printk, build, screenshot); proposal due Fri Oct 30.

### 2026-10-28 — Phase 2: scx_kairos skeleton builds
**Did:** Copied `scx_simple.bpf.c` / `scx_simple.c` from `~/linux-src`
(v7.0) to `scheduler/scx_kairos.bpf.c` / `scheduler/scx_kairos.c`. Renamed
`simple_*` → `kairos_*`, ops name `"kairos"`, skeleton `scx_kairos`; policy
unchanged. Added `bpf_printk("kairos enqueue: pid=%d comm=%s enq_flags=0x%llx")`
at the top of `kairos_enqueue`. Added `scheduler/Makefile` and
`scheduler/.gitignore` (`build/`, `scx_kairos`). Not committed yet (working
tree on top of 18a2b7f).
**Found:** `make -C scheduler` builds with no warnings (clang 21.1.8, gcc
15.2.0); a second `make` does nothing. `./scheduler/scx_kairos -h` prints the
help (no load). Disassembly of `kairos_enqueue` shows the printk as `call 6`
(bpf_trace_printk) with `p->pid` at offset 0x770 and `p->comm` at 0x990.
The scheduler was **not loaded**, so the verifier has not checked it yet.
**Interpretation:** Build-only result. The verifier runs at load time
(`SCX_OPS_LOAD`), so verifier problems, if any, will show up on the first
`sudo ./scheduler/scx_kairos`. Guess: it passes, because scx_qmap passes
`p->comm` and `p->pid` to bpf_printk the same way.
**Decisions:** Changes the 2026-10-27 decision to build inside
`~/linux-src/tools/sched_ext`: we build in the repo with our own Makefile,
which reuses the upstream build outputs. Reason: the upstream rules compile
`$(sched).c` from their own directory, so using them directly would mean
copying our sources into the kernel tree.
**Problems:** None in the build.
**Next:** Load scx_kairos, capture `trace_pipe` output (screenshot for the
progress report), check `/sys/kernel/sched_ext/root/ops` = `kairos`, unload;
then remove or gate the printk before phase 4 measurements.

### 2026-10-28 — How the scx_simple policy works (code walkthrough)
**Did:** Walked through the policy in `scheduler/scx_kairos.bpf.c` and checked
the kernel side in `~/linux-src/kernel/sched/ext.c` (slice accounting, slice
expiry, keep-running path). No code changes.
**Found:** (from reading code, not measured) The policy has no wakeup
preemption: a woken task with no idle CPU waits in the vtime-ordered shared
queue until some CPU's task blocks or uses up its 20 ms slice. vtime only
matters when there is a queue. Details in section 4 (slice mechanics).
**Interpretation:** scx_simple is close to our planned "never preempt" static
mode with a 20 ms slice. The worst-case wait for a woken latency-critical task
is about one slice, which is the delay KairOS's LATENCY mode is meant to remove.
Guess until E1 measures it.
**Decisions:** Per-task runtime for the Feature B budget will come from
timestamps, not from the remaining slice.
**Problems:** VS Code IntelliSense can't find `scx/common.h` (it doesn't know
the Makefile's include paths); the build itself is fine.
**Next:** As in the previous entry: load, screenshot trace_pipe, unload.

### 2026-10-28 — Cost of the idle-CPU search in select_cpu
**Did:** Read `scx_select_cpu_dfl` and `select_task_rq_scx` (v7.0) to answer
whether the idle-CPU search is wasted work under high load. Removed the course
reference from the `scx_kairos.bpf.c` header comment.
**Found:** (from reading code, not measured) The search reads a kernel-kept
bitmask of idle CPUs; on this VM it is a few bit operations, and it fails
fastest exactly when all CPUs are busy. See section 4.
**Interpretation:** Under high load the search is likely a negligible part of
a wakeup (guess until measured). Removing it would not save work, because idle
CPUs must be found and woken somehow; it would only move the search into
enqueue and add an IPI and a trip through the shared queue.
**Decisions:** Keep the idle fast path. Measure per-callback cost in E7.
**Next:** Unchanged: load, screenshot trace_pipe, unload.

### 2026-10-28 — scx_kairos loads; enqueue printk visible
**Did:** Recorded the idle-search checks as an open question (section 9, for
E7). Loaded scx_kairos four times for a few seconds each (smoke tests; logs
kept only in the session scratchpad, nothing under `results/`). Code still
uncommitted (on top of 18a2b7f).
**Found:**
- The verifier accepted the program: `/sys/kernel/sched_ext/state` = enabled,
  `/sys/kernel/sched_ext/root/ops` = `kairos`; dmesg: `BPF scheduler "kairos"
  enabled` / `disabled (unregistered from user space)`.
- `trace_pipe` shows our printk, e.g.
  `systemd-udevd-499 [002] dN.31 47809.532992: bpf_trace_printk: kairos
  enqueue: pid=499 comm=systemd-udevd enq_flags=0x0`. Flag values decoded in
  section 4.
- Idle VM, loader stats after ~2 s: local=1286 global=78 (run 4); after ~7 s:
  local=2570 global=351 (run 1, while trace_pipe was being read). First second
  has more global enqueues (all tasks are enqueued once when the scheduler
  takes over).
- Stopping: direct SIGINT → exit 0 + `EXIT: unregistered from user space`.
  `sudo timeout -s INT` → kernel unregisters cleanly, but the loader dies of
  SIGKILL (exit 137) within ~1 s, without printing `EXIT:`.
**Interpretation:** The enqueue flags match the code reading: `0x0` for
re-enqueue after slice expiry (the trace's `N` = need-resched on the task
enqueuing itself), and no CPU_SELECTED bit for pinned kworkers. The SIGKILL
under `timeout` is a sudo-rs/timeout interaction, not a scheduler bug (guess;
cause not found).
**Decisions:** Scripts stop the scheduler with `sudo pkill -INT -x scx_kairos`,
not `sudo timeout`.
**Problems:** See above (exit 137 under `sudo timeout`).
**Next:** User takes the phase 2 screenshot (loader + trace_pipe side by
side); commit; then phase 3 (workloads: gRPC server, open-loop loadgen, zstd,
harness). Remove or gate the printk before phase 4 measurements.

### 2026-10-28 — Phase 2 complete: manual run, Ctrl-C unload
**Did:** Ran `sudo ./scheduler/scx_kairos` and
`sudo cat /sys/kernel/debug/tracing/trace_pipe` in two terminals (callback
output captured for the progress report) and unloaded with Ctrl-C. Committed
the phase 2 work (scheduler/ + docs) on branch `phase2-skeleton`, in the same
commit as this entry.
**Found:**
- Ctrl-C → `EXIT: unregistered from user space` (clean unload).
- Loader stats over ~14 s (cumulative): from local=2715 global=201 to
  local=10251 global=1730. Global grew ~10–70/s, except two seconds: +199
  (local +1028) and +1056 (local +638).
- All sampled trace lines had enq_flags=0x100009 (wakeup, no idle CPU found).
  The enqueued tasks were mostly the observation path itself: `cat` (the
  trace reader), `sudo`, `kworker/u16:*`, `sshd-session`, the VS Code server
  (`code-…`, `tokio-runtime-w`) and `node`.
**Interpretation:** Matches the scx_simple design: idle-CPU fast path most of
the time, enqueue only when no CPU was idle. The trace largely shows its own
observers: reading and displaying trace_pipe wakes `cat`, `sudo`, (likely) the
kworkers that push pty output, and `sshd-session`; each wakeup is an enqueue
and another trace line (the kworker role is a guess). The two spike seconds
mean all CPUs were briefly busy; the cause is not visible in this output.
**Decisions:** Phase 2 done. The printk stays until phase 4 starts, then is
removed or gated (it costs time and its readers add load, as seen here).
**Next:** Phase 3: gRPC server, open-loop load generator, zstd batch job,
experiment harness.

### 2026-10-29 — Phase 3: workloads and harness; smoke run S1
**Did:** Installed protoc (apt) and the Go protoc plugins. Wrote
`workloads/` (proto, generated `kairospb/`, `grpcserver`, `loadgen`,
Makefile, `batch/make_input.sh`, `batch/zstd_loop.sh`),
`experiments/run.sh`, `experiments/configs/smoke.conf` and
`analysis/summarize_run.py`; `scripts/meta.sh` now records `sched_ext_ops`.
Created `~/kairos-data/zstd-input-64M.tar`. Ran S1. Not committed (working
tree on top of 4edc764).
**Found:**
- Quick checks on the idle VM: Go's `time.Sleep` wakes ~1.2 ms late for
  sub-ms sleeps; `clock_nanosleep` 60 µs late at 100 µs; vCPU hiccups
  (section 8).
- Server with GOMAXPROCS=20: GC stalls, p99 76 ms at 2000 req/s;
  GOMAXPROCS=4: 0.9 ms (section 4).
- S1, quiet / burst / quiet phases (one rep, printk on):
  EEVDF p99 66.5 / 144.5 / 12.2 ms, batch 338 / 433 / 690 MB/s;
  scx_kairos p99 50.4 / 504.3 / 54.8 ms, batch 349 / 231 / 307 MB/s.
  Context switches 40.6k/s vs 20.7k/s; system time 18.5% vs 24.8%. Send lag
  p99 in the burst 12 ms vs 175 ms. EEVDF's per-second p99 ranged 4–295 ms
  within the run. scx_kairos counters: local=21, global=403774 (all CPUs
  busy, so almost every wakeup went to the shared queue).
**Interpretation:** The harness works end to end and stops everything
cleanly. The numbers can't be compared yet: one rep, printk on, large
variation within a run, and the load generator is itself delayed by the
scheduler under test. Guess: scx_kairos's burst tail comes from having no
wakeup preemption and 20 ms slices next to 4 CPU-bound zstd jobs, so a woken
worker (or the sender) waits for a slice to end.
**Decisions:** Server: locked worker threads, GOMAXPROCS left at Go's
default. Loadgen: latency from the intended send time, actual send time
recorded. Phase 3 done (one run produces latency and throughput logs).
**Problems:** Stopping the batch loop via `setsid` failed (fixed with trap +
background zstd + `wait`). `pkill -f zstd_loop.sh` matched and killed the
shell that ran it (don't use `pkill -f` with a pattern that matches the
caller). GOMAXPROCS/GC stalls (fixed by keeping Go's default).
**Next:** Decide how to isolate the load generator; measure the noise floor
(repeated EEVDF runs); phase 4: remove or gate the printk, then timestamps,
runnable count, wakeup rate, wait time, BPF timer + EWMAs, forced modes,
victim preemption.

### 2026-10-29 — Co-author trailer removed; phase 2 commit rewritten
**Did:** Turned off Claude Code's commit/PR attribution (`attribution` in
`~/.claude/settings.json`). Rewrote the phase 2 commit's message without the
`Co-Authored-By` line: 4edc764 is now 210079b, same tree (82e160a), only the
message differs. `phase2-skeleton` moved with it.
**Found:** S1's `meta.json` files and its section 5 row record 4edc764; that
code is identical to 210079b.
**Problems:** 4edc764 is on GitHub, so `origin/main` still has the old commit
until main is force-pushed.
**Next:** Force-push main (`git push --force-with-lease origin main`).

### 2026-10-29 — Load generator isolation; noise floor (N1–N3)
**Did:** Committed phase 3 (180b6a7, branch `phase3-workloads`). Pinned the
server and batch to CPUs 0–2 and the load generator to CPU 3 (3d74753);
recorded other processes' CPU, loadgen CPU and server GC per run (3d74753,
ce5a697); added `aggregate.py` and `episodes.py`. Ran N1, N2 (10 reps each)
and N3 (repeat of N1 with GC logging).
**Found:** Section 6.1. p50 and batch MB/s vary ~10% across reps; p99 and
p99.9 vary 3–50× (CV 65–210%), driven by slow episodes. Median p99 at
3000 req/s with 4 zstd: 109 ms (N1) vs 38 ms (N3). Episodes don't line up
with server GC beyond chance. Pinned loadgen: send lag p99 2–5 ms median.
**Interpretation:** Single-run tail numbers are meaningless here; only
within-batch, interleaved, many-rep comparisons are. The episode cause is
unknown (host stalls, other guest processes, or slow backlog drain; all
guesses).
**Decisions:** Load generator on its own CPU with GOMAXPROCS=4 (no root
needed; chrt would need sudo on every run and preempts the measured work;
the Mac host adds network latency). ≥ 10 reps for E1, report medians and
ranges. Earlier "GC isn't the cause" from one GOGC=100 vs 400 pair was not
valid evidence at this noise level; N3 is the evidence.
**Problems:** `analysis/__pycache__` made N2/N3's `git_dirty` true (false
positive; ignored from cf7d8bc). The root cause of episodes is not found.
**Next:** Phase 4: gate the printk, timestamps and wait time, runnable count,
wakeup rate, BPF timer + EWMAs, forced modes, victim preemption; then E1.

### 2026-10-29 — Phase 4: load signal, forced modes, victim preemption; E1
**Did:** On branch `phase4-load-signal`: scx_kairos with runnable count,
wakeup count, wait time, 1 ms BPF timer with EWMAs, `--never-preempt` /
`--force-preempt` with victim preemption (SCX_ENQ_PREEMPT), timestamp-based
vtime charging, printk gated behind `--trace`, loader stats CSV (bd50425).
Harness: labeled scheduler entries, `sched_stats.csv`. Analysis:
`compare.py`, percentiles that count failed requests. Ran the E1 pilot and
E1 (4 configs × 10 reps, 5ec281d).
**Found:** Section 4 (mechanism, verification: BPF runnable count = kernel
`procs_running`), section 6.2 (E1). always-preempt had the lowest p50 at
every rate (8–10 of 10 reps); p99 always ≈ never3; never (20 ms slices) had
more batch throughput at 2000 req/s (9/10). EEVDF failed from 2000 req/s.
**Interpretation:** No latency crossover in this range; the trade-off is
latency vs batch throughput, and slice length explains most of the tail
difference. The victim rule probably keeps latency tasks from preempting
each other (guess). Measured load depends on the mode.
**Decisions:** Victim = busy CPU with the largest vtime, only if ahead of
the woken task. LATENCY slice 3 ms, BATCH 20 ms (tunable). Phase 4's
done-criterion ("E1 shows the crossover") is not met as stated; how to go
on is open (section 9).
**Problems:** `NR_STATS` clashed with vmlinux.h. Printf formats for the
skeleton's u64 (`unsigned long` on arm64). The E1 pilot's EEVDF run hit
memory pressure from the VS Code C/C++ extension (invalid). Failed requests
were invisible in loadgen's percentiles (now counted as infinite).
**Next:** Decide how to look for the crossover (section 9: lower load,
latency-only contention, or a throughput target), then phase 5.

### 2026-10-29 — Realism of the CPU split; what the load signal sees
**Did:** Answered questions on the CPU split, GC and the state of the
project. Checked the loopback receive path in `~/linux-src` and computed
per-phase load and wakeup rates from the E1 stats.
**Found:** Loopback receive work runs on the sending CPU (section 8). Under
never-preempt, runnable per CPU stays at 2.3–2.5 from 500 to 3000 req/s;
wakeups/s rise with the request rate but differ 2–2.6× between modes
(section 6.2).
**Interpretation:** The split mirrors remote clients and keeps the
instrument out of the scheduler's way; its remaining gaps are loopback vs
NIC and the small CPU count. The runnable count is the wrong switch signal
while batch jobs fill the CPUs (section 7).
**Decisions:** Keep the CPU split. Choose the phase 5 switch signal after
E1b.
**Problems:** None.
**Next:** E1b (no batch, more workers or shorter requests; plus 1–2 zstd
jobs), then decide between the original hypothesis and the trade-off
framing (section 9).

### 2026-10-29 — E1b pilot: request threads competing
**Did:** Added `SERVER_GOMAXPROCS` to `run.sh`. Ran three one-rep pilots
(eevdf, never, always): no batch with GOMAXPROCS 3 and 16, and 2 zstd with
GOMAXPROCS 3, rates up to 6000 req/s (2d885f7).
**Found:** Section 6.3. The load generator keeps up to 6000 req/s. Without
batch and with GOMAXPROCS 3, load stays ≤ ~1.1 per CPU and the server
saturates near 4000–5000 req/s; with GOMAXPROCS 16, load reaches 4.7 and
the server sustains 5000. With 2 zstd, always-preempt halved batch
throughput at 1000 req/s.
**Interpretation:** Go's own scheduler hid the request queue from the
kernel in E1 and in the default no-batch setup (section 7). E1b must use
GOMAXPROCS above the CPU count to test request-vs-request preemption.
**Decisions:** Proposed E1b: no batch, GOMAXPROCS 16, eevdf / never /
never3 / always, 1000–5000 req/s, 10 reps (~50 min). Waiting for the IDE
to be idle before running it.
**Problems:** Two pilot runs disturbed (memory pressure in EEVDF-gmp16;
VS Code `node` at 32% of a CPU in always-z2).
**Next:** Run E1b; then choose the phase 5 switch signal and framing.

### 2026-10-29 — E1b: preempting request threads
**Did:** Ran E1b (no batch, server GOMAXPROCS 16; eevdf, never, never3,
always; 10 reps; 3a9b17e). Split latency into send lag, queue and service
time for E1 and E1b.
**Found:** Section 6.4. always has the higher p50 in every rep at every
rate; its p99 is worse than never3's at 3500–5000 req/s. The cause is
measured: workers are preempted mid-request (service time 0.56 → 2.6 ms).
In E1 the server-side time was equal across modes; the gain was on the
gRPC path outside the worker.
**Interpretation:** The best fixed policy depends on the victim's class
(batch vs request thread), not on the load level (section 7). This points
to class-aware preemption (Feature A) as the main mechanism.
**Decisions:** None yet; proposed to test batch-only victims before
building the load switch (phase 5).
**Problems:** None. The never config's tails include the load generator's
own delay (send lag p99 up to 26 ms in E1b, 60 ms in E1).
**Next:** User decision on reordering: batch-only victim rule + a mixed
experiment (2 zstd, GOMAXPROCS 16) first, or phase 5 as planned.

### 2026-10-29 — Batch classification; E1c (batch-only victims)
**Did:** Added per-task batch classification (EWMA of voluntary stops) and
`--victim batch` (ed5d9ff); checked it live. Ran E1c: 2 zstd + server with
GOMAXPROCS 16; eevdf, never, never3, always, batchvic; 10 reps (0b25c6b).
**Found:** Section 6.5. batchvic has the lowest p50 at every rate (8–10/10
paired up to 4000 req/s), workers are not preempted mid-request (service
time flat at 0.5 ms), 3–6× fewer preemptions than always, more batch
throughput than always but 13–27% less than never3. Plain always crosses
never3 in p50 at ~4000 req/s. 2 batchvic reps collapsed at 4000–4500.
**Interpretation:** The victim's class is what decides whether preemption
helps; protecting request threads works at every load. The design is
viable with class-aware preemption at its core; the load switch then
manages the batch throughput trade-off. The classifier likely mislabels
workers that drain a backlog (section 7).
**Decisions:** Pulled the batch part of Feature A ahead of phase 5 (user
agreed). Keep `--victim any` as the default until the overload failure is
understood.
**Problems:** Test script deadlocked on a stale bash job number during the
live check (the scheduler itself was fine; unloaded by hand).
**Next:** Count class transitions to confirm the overload mechanism; make
the classification robust under backlog; rerun 3500–4500 req/s with more
reps; then the load switch (phase 5) for the throughput trade-off.

### 2026-10-30 — Proposal draft
**Did:** Wrote the 2-page proposal in the USENIX template
(`docs/proposal/proposal.tex`, with `usenix-2020-09.sty` from usenix.org):
motivation, proposed policy (load signal, two modes with hysteresis,
class-aware preemption, budget and max wait, prediction as an extension),
evaluation plan (workloads, metrics, baselines, E1–E8), expected outcomes
and challenges. No implementation status, as asked.
**Found:** —
**Interpretation:** The E1b/E1c lesson (preempt batch, not request threads)
is in the design as class-aware preemption, stated as a design argument,
not a result.
**Decisions:** Dropped ghOSt and Etsion et al. from the references to fit
two pages; NAPI kept as a one-line analogy.
**Problems:** No TeX in the VM, so the page count is estimated
(~1,100 words + 11 references), not checked.
**Next:** Compile (Overleaf or a local TeX install) and check it fits.
Update (same day): the user has the proposal; `docs/proposal/` removed
from the repository at the user's request (recoverable from 16bad51).

### 2026-10-30 — Class-change diagnostics; run-time classifier
**Did:** Removed the proposal from the repo. Added class-change counters
and `--trace-class` (e1f2698). Ran the overload diagnostic (DC1) with the
EWMA classifier, replaced it with "20 ms of CPU since last sleep"
(3f3d11e), ran it again (DC2).
**Found:** Section 6.6. EWMA: 45 server threads relabelled 488 times,
mostly in the rep that collapsed. Run-time rule: 42 relabellings, but one
rep still collapsed at 4500 req/s.
**Interpretation:** Mislabelling was real but not the whole cause of the
collapses; near capacity every policy can collapse (section 7).
**Decisions:** Run-time classifier replaces the EWMA (section 4); "wakes
others" not used for now (unreliable under Go when all Ps are busy).
**Problems:** `trace_pipe` busy once; a `pkill -f` pattern matched its
own shell and killed it (exit 144). Use a timed reader instead.
**Next:** E1c again with the new classifier (never3, always, batchvic;
10 reps).

### 2026-10-30 — E1c2: run-time classifier
**Did:** Ran E1c2 (never3, always, batchvic; 10 reps; 661c254). Checked
zstd's threads and their sleep pattern.
**Found:** Section 6.7. No failures; slow runs similar across configs.
batchvic: lowest p50 vs never3 up to 4000 req/s; vs always better at
3500–4500, tied/worse at 2000–3000; more batch than always everywhere.
zstd 1.5.7 has 4 threads; compression threads sleep every ~50 ms.
**Interpretation:** The overload failure is gone in this batch, at the
price of preempting zstd less (section 7). E1c's always victim shares
were inflated by the old classifier.
**Decisions:** Keep the run-time classifier; consider adding a burst
average (Feature A) later.
**Problems:** None.
**Next:** Decide phase 5's objective (user), then build the switch.
