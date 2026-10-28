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

---

## 5. Experiment log
One row per experiment run batch. Raw data lives in `results/<date>-<name>/`
together with `meta.json` (git commit, kernel, VM config, parameters).

| ID | Date | Commit | Config(s) | Workload / pattern | Reps | Data path | Status / one-line result |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

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

---

## 7. Explanations and insights
(The "why" behind each result. This is what the report is graded on.)

Expected explanations to test, not yet confirmed:
- Preemption pays off at low load (no queue, immediate service) and stops paying
  off at high load (switch cost + cache pollution + broken batching × queue length).
- Adaptivity helps only when bursts last longer than the scheduler's reaction time.
- Persistence is a strong burst predictor because bursts persist; ML should help
  mainly for periodic bursts.

---

## 8. Limitations
- VM: vCPUs are macOS threads on mixed P/E cores; no SMT/NUMA topology; fanless
  thermal throttling.
- Synthetic service (gRPC CPU loop) may not reflect real request costs; mitigated
  with Redis runs.
- Trace replay is time-compressed and rate-scaled.

---

## 9. Open questions
- Exact LOW/HIGH crossover on this VM? (E1)
- Which preemption mechanism is most reliable?
- Can wakers be identified reliably, including interrupt-driven wakeups?
- Does ramping slices during a switch reduce transition spikes vs a hard switch?
- Does ML beat persistence for any pattern besides periodic?

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
