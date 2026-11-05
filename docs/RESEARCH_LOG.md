# Research Log: KairOS — Load-Adaptive Wakeup Preemption with sched_ext

Individual project. Author: Aayush Nair.

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

### 2.8 Papers to cite
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


### 2.9 Time-slice length (read 2026-10-30)
Checked in the sources on this VM where possible:
- Linux EEVDF: base slice 0.7 ms x (1 + log2(min(CPUs, 8))) = 2.1 ms on
  4 CPUs (`kernel/sched/fair.c`, `sysctl_sched_base_slice`,
  `get_update_sysctl_factor`). Since 6.12 a task can request its own
  slice through `sched_attr.sched_runtime`, clamped to 0.1–100 ms
  (LWN 969062). CFS before it used a flexible slice: a 6 ms target
  latency divided among the runnable tasks, with a 0.75 ms minimum (both
  scaled like the base slice).
- sched_ext's default slice is 20 ms (`SCX_SLICE_DFL`).
- scx_bpfland 1.1.4: at most 1 ms, divided by the number of tasks
  waiting (`task_slice()`); no minimum by default.
- scx_lavd 1.1.4: 10 ms target latency x active CPUs / queued tasks,
  clamped to 0.5–5 ms (`sys_stat.bpf.c`); a task whose average run is
  longer gets a boosted slice when the system isn't loaded.
- This kernel has HZ = 1000; sched_ext checks slice expiry at the tick,
  so slices are effectively whole milliseconds.
- Microsecond-scale RPC systems preempt far more often: Shinjuku
  (NSDI 2019) every 5 µs; LibPreemptible (HPCA 2024, arXiv 2308.02896)
  adapts its quantum from windowed statistics and reports ~10% more
  best-effort throughput at the same p99 target when colocated (their
  claim, not checked). Our requests do 500 µs of work, so the relevant
  range is milliseconds.
- Cost of a switch: direct cost a few µs; the indirect cost of refilling
  caches ranges up to hundreds of µs once the working sets exceed the
  cache (Li, Ding, Shen, ExpCS 2007).
- Ours: never (20 ms) vs never3 (3 ms) in E2 with 2 zstd jobs (6.9):
  busy-phase p50 9–10 vs 4–6 ms, p99 131–154 vs 35–53 ms, batch
  throughput 0–12% lower with 3 ms.

Sources: https://lwn.net/Articles/969062,
https://www.usenix.org/conference/nsdi19/presentation/kaffes,
https://arxiv.org/abs/2308.02896,
https://www.cs.rochester.edu/~kshen/papers/expcs2007.pdf

### 2.10 How colocation systems get latency and batch throughput together (read 2026-10-30)
Question: in E2, BATCH-like configurations got the most batch throughput
and preemption the best latency; how do deployed and research systems
get both? From papers' abstracts, summaries and the scx sources on this
VM (details beyond those not checked):
- They mostly don't time-share a CPU between the two classes; they split
  CPUs and move the split with load:
  - Heracles (Google, ISCA 2015): separate cores (cpusets), plus cache,
    memory-bandwidth and network isolation; a controller reads the
    service's tail latency and load every 15 s and gives batch work
    resources only while there is slack to the SLO. ~90% average
    utilization without SLO violations.
  - PerfIso (Microsoft Bing, ATC 2018; >90,000 servers): "CPU blind
    isolation" keeps a buffer of idle cores for the bursty service and
    lets batch jobs use the rest; utilization 21% -> 66% off-peak
    without tail impact.
  - Shenango (NSDI 2019) and Caladan (OSDI 2020): a dedicated core
    reallocates cores between applications every 5-10 µs; the signal is
    queueing delay (packets or threads still waiting since the last
    check), not a per-event threshold. Caladan uses core allocation only
    (no hardware partitioning) and reports 52 µs vs 580 ms tail latency
    against PARTIES during load shifts (their claim).
  - Meta ads (engineering blog, 2026-07-13): a custom sched_ext policy
    soft-partitions CPUs into a pool for request-path threads and one for
    the rest, sizes the pools with load heuristics, chooses threads by
    domain knowledge: -28% p99, then -60% more with later policy
    updates. scx_layered (Meta) does the same in general form: layers
    with CPU budgets, unused CPUs lent to other layers.
  - Why it works: a woken service thread finds an idle CPU (no wait, no
    preemption) while batch threads run long, uninterrupted slices on
    their own CPUs (warm caches, few switches). The cost is CPUs kept
    idle as a buffer, which the controller sizes by load.
- Their signals are about the service, not single wakeups: tail latency
  vs SLO (Heracles), queueing delay (Shenango, Caladan), idle-core count
  (PerfIso), load (Meta).
- Where they do time-share, they shorten rather than cut: scx_lavd
  shrinks the running task's remaining slice (`scx_bpf_task_set_slice`)
  and sends an IPI only if it has already overrun; slices 0.5–5 ms
  scaled by queued tasks (2.9). scx_bpfland gives at most 1 ms divided
  by waiting tasks, and interactive tasks (high voluntary switch rate) a
  separate, higher-priority queue.
- They identify latency-critical work explicitly or by structure: per
  job (Heracles, PerfIso), per thread from domain knowledge (Meta), or
  scx_lavd's latency criticality: high wake and wait frequency, short
  runs, propagated along waker/wakee chains, which catches threads in
  the middle of a request path. Linux's SCHED_BATCH/SCHED_IDLE and
  cgroup `cpu.idle` let users mark batch work; under EEVDF the only
  difference left for SCHED_BATCH is that it doesn't preempt normal
  tasks on wakeup (Zijlstra, LKML 2024).
- Application side: Tales of the Tail (SoCC 2014) found background
  processes, request re-ordering by the scheduler, interrupt placement
  and connection-to-thread binding behind tails, and p99 rising 10x
  from 50% to 95% utilization. IX (OSDI 2014) batches only under
  congestion and bounds the batch size. Go services set GOMAXPROCS to
  their CPU allocation (container-aware by default since Go 1.25); our
  server runs GOMAXPROCS 16 on 3 CPUs on purpose (E1b) so the kernel
  sees the threads, which also adds Go-level queueing and spinning that
  a production setup wouldn't have.

Sources: https://research.google/pubs/heracles-improving-resource-efficiency-at-scale/,
https://www.usenix.org/conference/atc18/presentation/iorgulescu,
https://www.usenix.org/conference/nsdi19/presentation/ousterhout,
https://www.usenix.org/conference/osdi20/presentation/fried,
https://engineering.fb.com/2026/07/13/ml-applications/modernizing-the-meta-ads-service-with-an-open-source-kernel-scheduler/,
https://lkml.iu.edu/hypermail/linux/kernel/2407.1/02711.html,
https://syslab.cs.washington.edu/papers/latency-socc14.pdf,
https://www.usenix.org/conference/osdi14/technical-sessions/presentation/belay,
https://go.dev/blog/container-aware-gomaxprocs

### 2.11 Systems close to KairOS's split, and why some get more batch throughput (read 2026-11-02)
Question after E3: KairOS has the lowest latency in every block but 7–20%
less batch throughput; which similar systems get both, and how? From
abstracts, docs, artifact reviews, one engineering blog and the 7.0
kernel source on this VM (paper bodies mostly not read; numbers are the
authors' claims):
- **Linux SCHED_IDLE** is the closest mechanism in mainline. In 7.0's
  fair class a woken non-idle task always preempts a SCHED_IDLE one,
  without slice protection (`check_preempt_wakeup_fair()`:
  `PREEMPT_WAKEUP_SHORT`), and a CPU running only SCHED_IDLE tasks counts
  as idle when a woken task looks for a CPU (`sched_idle_cpu()` in
  `select_idle_sibling()` and the slow path). SCHED_IDLE weight is 3
  against 1024 for nice 0. The placement rule came from Viresh Kumar's
  2019 series: mean wakeup latency in rt-app fell from ~1,117 to ~103 µs
  with 5 SCHED_IDLE tasks running; Song Liu saw fewer long wakeups for a
  web service with an ffmpeg side job (LKML). That is KairOS's borrower
  preemption without a CPU split or a pool. Never measured here: all our
  runs used SCHED_BATCH, which only stops batch jobs from preempting.
  → Measured in E3-idle (section 6.21): about KairOS's batch throughput
  (+2%) at 2-3x its busy median.
- **Kernel QoS classes in production kernels** do the same with cgroups:
  Alibaba Cloud Linux's Group Identity (a second run queue tree, so
  high-identity tasks wake first, and an "SMT expeller" keeps
  low-identity ones off busy SMT siblings; vendor-reported <5%
  interference to online tasks at >60% CPU utilization) and openEuler's `cpu.qos_level` (offline groups
  become SCHED_IDLE and are throttled while online tasks run on the CPU;
  from the patch titles, offline tasks are throttled only on return to
  user space, so they don't hold kernel locks while throttled). Both use absolute priority on wakeup, not a CPU split.
- **UFS** (Bilodeau-Savaria et al., arXiv 2605.02377, May 2026), a
  sched_ext policy for PostgreSQL: two tiers by cgroup, background work
  runs only on idle capacity and is preempted at once by time-sensitive
  work, which is placed on a chosen CPU and kicked there. Against EEVDF,
  TPC-C (bursty tier) kept 75% of its solo throughput vs 40% under EEVDF
  in a 50:50 mix, with mean/p95 latency 4.4/11.4 ms vs 7.5/17.2 ms;
  SCHED_RR collapsed (183/1,398 ms). Their explanation: EEVDF stacks
  short bursty tasks on a few CPUs and balances too slowly; FIFO/RR lose
  fairness within the tier. They also had to handle priority inversion
  (a lock-holding background backend is boosted, from lock waits that
  PostgreSQL reports in a BPF map).
- **Meta's ads policy** (sched_ext, engineering blog 2026-07-13): two CPU
  pools sized by "load-based heuristics", threads assigned by domain
  knowledge; -28% p99 on ads retrieval vs CFS (6.4), then a further -60%
  from user-space policy updates, and 1.6% lower CPU utilization. Pool
  sizing, lending and preemption details aren't published.
- **Systems with spare cores**: PerfIso (keeps a buffer of idle cores),
  Heracles (moves cores every 15 s from tail latency vs SLO), Shenango
  and Caladan (move cores every 5–10 µs from queueing delay; Shenango
  kept p99.9 under 125 µs through an extreme load shift, Caladan reports
  52 µs vs 580 ms against PARTIES during resource shifts). They get both
  because the batch CPUs are never touched and the buffer is small when
  reallocation is fast. Perséphone (SOSP 2021, DARC) goes further:
  keeping a few cores idle for short requests sustains up to 2.3x more
  load than Shinjuku and 1.3x more than Shenango, at a 5% throughput cost ("when idling is
  ideal"). McClure et al. (NSDI 2022) compared core-allocation policies
  (by queueing delay, arrivals or utilization): several do about equally
  well, and for small tasks a static allocation often beats reallocating
  at the same efficiency.
- **Why bpfland gets 6–13% more batch than KairOS here** (from its source,
  section 2.9, and E2k/E3 numbers): it never preempts, but its slices are
  short (1 ms / waiting tasks) and its deadlines favour tasks that slept,
  so a woken service thread waits at most about one short slice on any of
  the 3 CPUs. KairOS preempts a borrower on every LC wakeup that finds no
  idle CPU: 21% more context switches than bpfland in E3, ~0.07 CPUs more
  idle on CPUs 0–2 and ~0.02–0.09 CPUs more server CPU.
- **ghOSt** (SOSP 2021) and **Nest** (EuroSys 2022) are context: ghOSt's
  claim is parity with the kernel scheduler while moving policy to user
  space (and 5–30% better tail latency than Google's MicroQuanta for
  Snap); Nest gains 10%–2x on light loads by keeping tasks on a few warm
  cores, little when all cores are busy (our case).

Takeaways for KairOS:
- On 3 CPUs nothing can give the service spare cores, so the systems that
  get both (PerfIso, Caladan) don't apply directly; what remains is how
  cheaply the batch work is displaced. Linux's own answer is SCHED_IDLE,
  which must be the baseline KairOS beats.
- The cost per displacement is what bpfland avoids: it bounds waits with
  short slices instead of preempting. A middle ground: preempt only when
  a wait would exceed the target, otherwise shorten the borrower's slice
  (lavd's `scx_bpf_task_set_slice()` approach).

Sources: kernel/sched/fair.c (v7.0, ~/linux-src),
https://lkml.rescloud.iu.edu/1906.3/03218.html,
https://lkml.iu.edu/hypermail/linux/kernel/1905.1/06262.html,
https://lab.nexedi.com/kirr/linux/-/commit/17346452b25b98acfb395d2a82ec2e4ad0cb7a01,
https://www.alibabacloud.com/blog/600068,
https://help.aliyun.com/en/alinux/user-guide/group-identity-feature,
https://mailweb.openeuler.org/archives/list/kernel@openeuler.org/thread/VO6RYWGEYPV6UT2ZCMLZEFG63OMDKNI7,
https://arxiv.org/abs/2605.02377,
https://engineering.fb.com/2026/07/13/ml-applications/modernizing-the-meta-ads-service-with-an-open-source-kernel-scheduler/,
https://www.usenix.org/conference/nsdi19/presentation/ousterhout,
https://www.usenix.org/conference/osdi20/presentation/fried,
https://www.microsoft.com/en-us/research/?p=782413,
https://sysartifacts.github.io/sosp2021/summaries/perséphone,
https://www.usenix.org/conference/nsdi22/presentation/mcclure,
https://research.google/pubs/ghost-fast-and-flexible-user-space-delegation-of-linux-scheduling/,
https://sysartifacts.github.io/eurosys2022/summaries/nest

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

**Revised 2026-10-30 (user decision): switch against a latency target,
not load thresholds.** E1/E1b/E1c showed that BATCH mode at high load
makes requests slower and only buys batch throughput, and that the
runnable count doesn't track request load. New rule (timer, every 1 ms):
```
late_frac = EWMA over ~64 ms of (woken tasks that waited > TARGET) / (woken tasks)
BATCH   and late_frac > LATE_MAX                  -> LATENCY  (fast exit)
LATENCY and in mode >= HOLD and late_frac <= LATE_MAX/2 -> BATCH  (cautious entry)
if a BATCH stint fails within HOLD: HOLD *= 2 (up to HOLD_MAX); else HOLD = HOLD_MIN
```
LATENCY = preempt (batch victims only, with `--victim batch`), 3 ms
slices; BATCH = no preemption, 20 ms slices. Defaults: TARGET 1 ms,
LATE_MAX 5%, HOLD 200 ms–5 s. Only wakeups on `--signal-cpus` count.

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

**Revised 2026-10-30 (with the soft CPU split, section 2.10):** the
predictor estimates the LC demand (CPUs) the pool must cover over the next
200 ms, from the last 500 ms of 50 ms demand samples and the BPF demand
EWMA; the BPF side uses a fresh prediction to raise (never lower) the
demand the pool is sized from, so stale or missing predictions fall back
to the reactive sizing. Inference runs in the C loader (a tree read from a
file every 50 ms) instead of a Python daemon: the loader already holds the
BPF maps, and a 50 ms loop through a second process would add IPC for
nothing. Training stays in Python (`ml/train_burst.py`).

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
- Adaptive switching (3bd9dff, a7fd0f6, 6206245): `--adaptive` with
  `--lat-target-us`, `--late-pct`, `--late-shift`, `--hold-ms`,
  `--hold-max-ms`, `--signal-cpus` (section 3.4, revised). `runnable`
  marks a task `woken` on SCX_ENQ_WAKEUP; `running` counts it in
  `lat_wakeups` (and `lat_late` if it waited longer than the target) when
  its CPU is in the signal mask; the timer turns the per-tick deltas into
  `late_frac` and runs `update_mode()`. Time per mode (`mode_ns`), mode
  switches and the late counts go to the stats CSV. Two fixes found in
  the pilots: the late share was first smoothed with weight 1/8 per tick
  (~8 ms, 50–100 wakeups), which crossed 5% in single bad milliseconds;
  now 1/64 (~64 ms). And an integer EWMA with weight 1/2^k cannot decay
  below 2^k − 1 (`avg >> k` rounds to 0): at x1024 and k = 6 that floor
  is 6.2%, above the 5% threshold, so the mode never left LATENCY; the
  late share now uses 2^20 fixed point (`LATE_ONE`).
- The latency signal counts all woken tasks on the signal CPUs, including
  zstd's main and I/O threads (~600 wakeups per pass). Excluding a batch
  process's light threads would need per-process classification.


- Diagnostics (`--diag FILE --diag-comm LIST`, e557e62): `running()`
  counts every wait for a CPU into a per-CPU `struct diag`, by task group
  and by why the task was waiting (`enum kairos_ready`): woken
  (`SCX_ENQ_WAKEUP` in `runnable()`), preempted or slice ran out (set in
  `stopping(p, true)`), other. Ran out vs preempted: the slice counts
  down with run time, so a task that ran at least the slice it started
  with (`p->scx.slice` in `running()`) ran out; shorter means it was
  preempted (our `SCX_ENQ_PREEMPT` sets the slice to 0; a higher class
  leaves it). Groups are task-name prefixes; `p->comm` is copied to the
  stack first, because the verifier allows only constant offsets into a
  `task_struct` but a bounded variable index into a stack buffer. Waits
  go into 8 buckets (< 50 µs ... ≥ 20 ms). The loader writes per-second
  deltas (one row per group and kind, plus a "run" row with CPU time).
  The old `woken` flag became `ready_kind == READY_WAKEUP`.
- The server names its worker threads `grpcworker` (`prctl(PR_SET_NAME)`
  after `LockOSThread`); other threads keep `grpcserver`. A worker that
  lands on the main thread isn't renamed (the main thread's name is the
  process name); 2 of 30 runs logged one.
- `grpcserver -schedlat` logs Go's `/sched/latencies:seconds` histogram
  per second, re-bucketed by each Go bucket's lower edge. Go samples 1 in
  8 goroutine transitions (`gTrackingPeriod = 8`, `runtime/proc.go`), so
  counts are 1/8 of the real number; shares are unbiased.
- The reply carries `recv_unix_ns` (handler got the request, wall clock)
  and `wake_ns` (worker finished -> handler running again), dbfae96.
  With the load generator's times they split the rest of the latency into
  in / wake / out (`analysis/split.py`).


- Soft CPU split (`--split`, 8b32051, be3eb2c): two DSQs (LC_DSQ,
  BATCH_DSQ). `split_rank[]` (rodata, from `--split-cpus`) orders the
  split CPUs; the first `nr_lc` are LC CPUs, CPUs outside the split act as
  LC CPUs. `dispatch()` takes from the CPU's own pool's queue first, then
  the other's. `select_cpu()` claims an idle CPU with
  `scx_bpf_test_and_clear_cpu_idle()`, own pool first, `prev_cpu` first.
  A woken LC task with no idle CPU preempts a batch task running on an LC
  CPU (`SCX_ENQ_PREEMPT`; off with `--no-borrow-preempt`), or any batch
  task if it can't run on an LC CPU. A batch task starting on an LC CPU
  gets `borrow_slice_ns` (`scx_bpf_task_set_slice()` in `running()`).
  Sizing in the timer: demand = LC run time per tick + one CPU per task in
  LC_DSQ, EWMA weight 1/2^`lc_shift`; nr_lc = ceil(demand + buffer) in
  [lc_min, lc_max], shrinking only if it would with `lc_hyst` more demand.
  Two fixes on the way: an ~8 ms demand average changed the pool 11-24
  times a second on an idle system (now ~64 ms: 0-4); and the verifier
  rejected `split_rank[cpu]` as unbounded because clang computed the
  address before the C range check, even with `& 63` (it knew the mask was
  redundant): `MEMBER_VPTR()` from `scx/common.bpf.h` does the check in
  inline asm.
- Task class `--class policy`: batch = SCHED_BATCH/SCHED_IDLE
  (`p->policy`); the harness sets it with `chrt --batch 0` when the config
  has `BATCH_POLICY=batch` (inherited by zstd). The fair class only stops
  SCHED_BATCH wakees from preempting (`wakeup_preempt_fair`, "BATCH and
  IDLE tasks do not preempt others"); weights are unchanged.


- ML for `--split` (02cb689, 549bf7c): every `--ml-period-ms` (50) the
  loader computes one demand sample, LC run time over the period / its
  length + mean LC tasks queued (from `lc_queued_ticks`, summed by the
  BPF timer, over `nr_timer_ticks`): the same quantity as the timer's own
  sample, averaged. Features: the last K = 10 samples newest first + the
  BPF EWMA; target: the largest sample of the next H = 4. `--ml-trace`
  writes the samples; `--ml-model` evaluates a tree and writes
  `ml_demand` then `ml_stamp` (= `nr_timer_ticks`) into the BPF bss; the
  timer uses the prediction while `nr_timer_ticks - ml_stamp <=
  ml_ttl_ticks` (100 ms by default). The two-word write is racy; a torn
  read can only pair a demand with a neighbouring stamp. The tree is a
  plain-Python CART (no numpy/scikit-learn on this VM), stored as text
  with children after parents so the loader can reject cycles.


- Idle kicks (`kick_idle`, 7f1a122; off with `--no-kick-idle`). Inserting
  a task into a user DSQ doesn't wake idle CPUs: the kernel doesn't know
  which CPUs consume that DSQ, so an idle CPU only looks again when
  something else wakes it (a task placed on its local DSQ, an interrupt).
  `select_cpu()` covers wakeups, but a task re-queued after a preemption
  or a slice end goes through `enqueue()` only. After every shared-DSQ
  insert, `enqueue()` now calls `scx_bpf_pick_idle_cpu(p->cpus_ptr, 0)`
  (finds an idle allowed CPU and clears its idle bit, so it isn't claimed
  twice) and `scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE)` (wakes it only if it
  is still idle; it then runs `dispatch()`), counted as STAT_KICK. The
  same pattern as `scx_central` and scx_bpfland's enqueue.
  → 2026-11-01: off by default, `--kick-idle` turns them on
  (`--no-kick-idle` is still accepted). The E2k rerun measured no gain
  (section 6.16).
- EWMA resolution (`ewma()`: `avg - (avg >> shift) + (sample >> shift)`).
  Both terms are truncated, so the average stops moving when it is within
  about 2^shift units of the sample. For the LC demand (CPUs x 1024) that
  is 1/16 CPU at `--lc-shift 6` (the default), 1/4 CPU at 8 and a whole
  CPU at 10. A slower pool average (e.g. ~1 s for E3) would need more
  fractional bits in the demand first. Found 2026-11-02, not changed.
- Counters for the improvement directions (d9d208a). `preempt_on[cpu]`
  counts preemptions by the CPU preempted (sched_stats.csv
  `preempt_cpus`, "n0;n1;..."). With `--diag`, a task that preempts is
  marked (`preempted_in`) and `stopping()` records how long that run
  lasted, per task group, in the wait buckets (diag.csv kind `preempt`;
  `analysis/diag.py` prints it). The timer samples each tick the idle
  split CPUs (`scx_bpf_get_idle_cpumask()`) and how many of them a queued
  task could run on (`bpf_for_each(scx_dsq, ...)`), integrated over time
  (`idle_cpus`, `idle_queued`). Verifier: a loop over CPUs inside the
  loop over a queue failed ("The sequence of 8193 jumps is too complex");
  each task is checked against the first idle CPU only, which is exact
  here (the tasks that may run on one split CPU may run on all).
- Delayed borrower preemption (`--preempt-wait-us D`, c366a77). A woken LC
  task that finds no idle CPU but a borrower it could preempt is queued
  in LC_DSQ (flag `delayed`) instead, any CPU that has gone idle since
  `select_cpu()` is kicked, and a one-shot BPF timer (`wait_timer_map`,
  armed once with a compare-and-swap on `wait_timer_armed`) fires after
  D. `wait_timerfn()` walks LC_DSQ, takes the first delayed task that has
  waited D, clears its flag and kicks a borrower with
  `scx_bpf_kick_cpu(SCX_KICK_PREEMPT)` (the CPU stops its task and runs
  `dispatch()`, LC_DSQ first). It fires again at once if more are
  overdue, and is re-armed for the next deadline otherwise. A preemption
  is avoided when an LC CPU frees up first (its LC task sleeps, or a
  borrow slice ends). `--preempt-min-run-us N`: tasks whose runs (wake
  to sleep, EWMA weight 1/4, `avg_run`) average at least N still preempt
  at once. Verifier, twice: an exact count of overdue tasks used to kick
  that many CPUs failed with "jumps too complex"; booleans pass (my
  reading, not checked in the verifier source: a branch decided by a
  known count marks the count precise, so every value is its own state). The
  timer's resolution is the host's: hrtimers in this VM, not measured.
- Sync wakeups (`--wake-sync`, 41b0745). As `scx_bpf_select_cpu_dfl()`
  (kernel/sched/ext_idle.c): on `SCX_WAKE_SYNC`, `prev_cpu` if idle, else
  the waker's CPU (`bpf_get_smp_processor_id()`, inserted into
  `SCX_DSQ_LOCAL` without claiming it) if the wakee may run there, its
  local DSQ is empty (`scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu)`),
  the waker isn't exiting (`PF_EXITING`) and some CPU is idle; added: the
  CPU must be in the wakee's pool and run a task of its class. The kernel
  also requires `cpus_share_cache(prev, waker)`; all 4 vCPUs share one L2
  here (`/sys/devices/system/cpu/cpu*/cache`). In the gRPC runs the
  loopback-TCP wakeups come from the load generator's CPU 3, where the
  server may not run, and Go's futex wakeups aren't sync, so little
  change is expected there (guess).
- `--borrower-pick first|prev|longest` (c583c7e): which borrower
  `split_pick_borrower()` returns; the delayed-preemption timer uses the
  same rule (with no task, any LC CPU).
- `--lc-queue percpu` (025d69e): per-CPU DSQs `LC_PCPU_DSQ(cpu)` = 16 +
  cpu, created in `init()`. A queued LC task goes to its CPU's DSQ (and
  that CPU gets `SCX_KICK_IDLE`, as only it looks there soon);
  `dispatch()` serves the CPU's own LC DSQ, then BATCH_DSQ, then other
  CPUs' LC DSQs. Verifier: a sum over a `bpf_for` loop gets a wider range
  every iteration, so the loop never converged (iterator depth 8184);
  masking the sum (`& 0xffffff`) inside the loop fixes its range.
- Redis mode (8790ee0): `loadgen -redis host:port,...` sends pipelined
  RESP GETs of random keys on `-conns` connections per server; a lock
  keeps the write order and the queue of pending ids in step, and a
  reader per connection matches replies in order. run.sh `SERVICE=redis`
  starts the redis-server instances pinned to SUT_CPUS (ports 6390+,
  no persistence) and fills them with `DEBUG POPULATE`. In a test without
  batch on EEVDF, the load generator kept up at 20k GET/s (send lag p99
  0.12 ms, 43% of its CPU) but not at 40k/s.
- Statistics for the final evaluation (`analysis/stats.py`, 671776a).
  Every metric per run comes from byburst.py's or bylevel.py's run_stats,
  and each config is paired with the reference by rep. The script gives
  the median paired difference with a percentile bootstrap 95% CI
  (10,000 resamples, seeded by metric and config). The test is an exact
  two-sided Wilcoxon signed-rank test: the rank sum against all 2^n sign
  patterns, counted with a dynamic program over doubled ranks so ties'
  average ranks stay integers, with zero differences dropped. I checked it
  against brute-force enumeration on 300 random cases with ties. Holm's
  correction runs within each metric. With 10 reps the smallest exact p
  is 2/1024 = 0.002, so Holm can only reject in families of at most 25
  tests. A family is therefore one metric (7 comparisons with the
  reference), where 10/10 passes Holm's first step (0.05/7 = 0.007). On
  E2a and E11 its medians match bylevel.py, byburst.py and sections 6.17
  and 6.23.
- Offline predictability check at 1 s (`ml/azure_predict.py`, 1018388):
  method in section 6.27.
- Duplicate borrower picks (3067b18), diagnostic only.
  - A borrower preemption marks its CPU (`cpu_ctx.preempt_pending`). The
    mark is set by `mark_pending()` before the insert or kick, and cleared
    when that CPU's current task stops or its next task starts.
  - Until the kicked CPU switches, it still looks like a borrower
    (`running_at` set, `batch` true), so a second LC wakeup can pick it
    again.
  - `dup_pick` counts preemptions of a marked CPU. `dup_alt` counts those
    where another candidate of the same kind was unmarked. Both are
    columns in sched_stats.csv.
  - `split_pick_borrower()` now scans every CPU to count the unmarked
    ones, but it makes the same choice as before.
  - A mark set just after the borrower stopped (a race) can't cause a
    false count. That CPU has `running_at` = 0, so it isn't a candidate
    until its next task starts, and that start clears the mark.
- PSI (3067b18).
  - run.sh saves /proc/pressure/{cpu,memory,io} as pressure_before and
    pressure_after.
  - vmsample.sh adds the stall totals (µs) every second, as
    psi_<resource>_<some|full>.
  - `runlib.pressure_stats()` turns them into stall ms per second;
    summarize_run.py and compare.py print them.
  - CPU "some" is high in every run by design: there are more runnable
    threads than CPUs (540–730 ms/s in DS). Memory "some" is the useful
    health signal.
- Kernel. Since the VM restart on 2026-11-04 it runs 7.0.0-42-generic.
  - unattended-upgrade installed it on 2026-11-03 at 06:43, and the
    restart booted it. 7.0.0-38 is still installed.
  - Every experiment up to G-nb ran on 7.0.0-38-generic (meta.json); FS
    and DS ran on -42.
  - apt-daily-upgrade.timer fires around 06:10–06:45 each morning.
- `--claim-borrower` (e11c590), off by default, so earlier configs keep
  their meaning.
  - `split_pick_borrower()` is now two parts:
    - `scan_borrowers(p, prev, free_only)` makes the old choice, and with
      `free_only` skips marked CPUs;
    - a second scan, only when the first choice is marked, gives the same
      rule's choice among the unmarked CPUs (`alt`).
  - `mark_pending()` became `claim_victim(cpu, alt)`.
    - It counts `dup_pick`/`dup_alt` as before: the counters keep their
      meaning with the option on.
    - Without the option it marks and returns `cpu`, as before.
    - With it, a marked `cpu` is replaced by `alt`, or by -1 if there is
      none. The task then takes the existing no-victim path: it is
      inserted into LC_DSQ and counted as `no_victim`.
  - The claim is `__sync_val_compare_and_swap(&preempt_pending, 0, 1)`.
    Of two wakeups that pick the same CPU at once, only one gets it. A
    lost race returns -1 and counts as `dup_pick` (once).
    `preempt_pending` became u32 for this, because BPF's atomics work on
    32 or 64 bits, not on bool. The plain stores that clear it in
    running() and stopping() can race with the compare-and-swap. That is
    harmless: either order leaves the CPU marked for at most one task
    switch.
  - Timer path (wait_timerfn()): an overdue task whose claim fails stays
    in LC_DSQ, no longer marked delayed. That is the same as finding no
    borrower before the change.
  - Load test (8 s; 4 SCHED_BATCH spin loops, 3 Python loops sleeping
    200 µs): loads, unloads cleanly. About 10,300 preemptions/s;
    `dup_pick` 4,077/s, of which 3,949 went to another borrower and the
    rest queued.
- `stats.py --skip-runs RUN ...` (e11c590) drops single runs. Pairing is by
  rep within each comparison (`set(vals[config]) & set(vals[ref])`), so a
  skipped run of a config costs only its own pairs. A skipped run of the
  reference drops that rep from every comparison with it. An unknown
  run name is an error.

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
| E2p | 2026-10-30 | a025a0d, a7fd0f6, 6206245 | never, batchvic, adaptive (`--adaptive --victim batch --signal-cpus 0-2`) | `e2-pilot.conf` (2 zstd) and `e2-pilot-z1.conf` (1 zstd): 5 s phases alternating 500 and 3500 req/s, GOMAXPROCS 16 | 1 | `results/2026-10-30-e2-pilot*/` | Functional pilots, IDE in use (other processes 20–100% of a CPU). With 2 zstd, BATCH always misses a 1 ms target. With 1 zstd and the fixed signal, adaptive is mostly BATCH when quiet and LATENCY when busy (section 6.8) |
| E2pt | 2026-10-30 | 652be42 | a5, a10 (`--adaptive --victim batch --signal-cpus 0-2`, `--lat-target-us` 5000 / 10000) | `e2-pilot-target.conf`: as `e2-pilot.conf` (2 zstd) | 1 | `results/2026-10-30-e2-pilot-target/` | Picks E2's 2-job targets. a10 stayed in BATCH all run (late ≤ 2%/s); a5 mostly LATENCY, BATCH tries ended within 1–2 s (section 6.9) |
| E2 | 2026-10-30 | 7eb796e | eevdf, never, never3, batchvic, adaptive (1 ms target); with 2 zstd a5, a10 (5 / 10 ms) instead of adaptive | `e2-z1.conf` (1 zstd), `e2-z2.conf` (2 zstd): 5 s phases alternating 500 / 3500 req/s, three times; GOMAXPROCS 16; IDE idle | 10 | `results/2026-10-30-e2-z1/`, `results/2026-10-30-e2-z2/` | No failed requests. 1 zstd: adaptive ≈ batchvic, quiet-phase batch +0–2.5%. 2 zstd: a10 never left BATCH (= never); a5 in between, busy p99 worse than batchvic and never3 (section 6.9) |
| ED | 2026-10-30 | e557e62 | as E2 (2 zstd): eevdf, never, never3, batchvic, a5, a10, with `--diag` | `e2-diag.conf`: E2's 2-job workload, groups grpcworker, grpcserver, zstd, loadgen, ksoftirqd | 3 | `results/2026-10-30-e2-diag/` | Server threads' waits are ≥ 95% wakeups; worker wakeup waits in BATCH mostly 2–5 ms. Tails 2–8x E2's for all configs incl. eevdf (section 6.10) |
| ES | 2026-10-30 | dbfae96 | as ED | `e2-split.conf`: as ED, server reports handler receive time and wake delay | 2 | `results/2026-10-30-e2-split/` | The slowest 1% are 81–97% "in" + "out" (gRPC/network path and client) in every config (section 6.10) |
| EC | 2026-10-30 | 52fe9bb | eevdf, never, batchvic, diagnostics off | `e2-control.conf`: as E2 (2 zstd) | 2 | `results/2026-10-30-e2-control/` | Tails as high as ED/ES without the diagnostics: the evening runs were noisier, not perturbed by the counters (section 6.10) |
| S1p | 2026-10-30 | 8b32051 | eevdf, never3, batchvicp (`--force-preempt --victim batch --class policy`), split (`--split --split-cpus 0-2 --class policy`) | `e2s-pilot.conf`: E2 2-job workload, zstd SCHED_BATCH, GOMAXPROCS 16 | 1 | `results/2026-10-30-e2s-pilot/` | Functional pilot, noisy (other processes 27–49%). Split and batchvicp collapsed in a busy phase (section 6.11) |
| S2p | 2026-10-30 | be3eb2c (+REPS 2) | as S1p + splitnp (`--no-borrow-preempt --borrow-slice-us 1000`), `--diag` on | `e2s-pilot-gmp16.conf`, `e2s-pilot-gmp3.conf` (server GOMAXPROCS 16 / 3) | 2 | `results/2026-10-30-e2s-pilot-gmp16/`, `-gmp3/` | Other-process CPU 3–137% between runs (VS Code server, the Claude CLI, kswapd): inconclusive. eevdf failed requests in 4/4 runs (section 6.11) |
| E2s | 2026-10-30 | a2ac442 | eevdf, never3, batchvicp, split | `e2s.conf`: E2 2-job workload, zstd SCHED_BATCH, GOMAXPROCS 16; Mac and IDE idle, start delayed 90 s | 5 | `results/2026-10-30-e2s/` | No failed requests; other processes 9–21%. split ≈ batchvicp on latency, +4% quiet batch MB/s, 63% fewer preemptions when quiet; vs never3 p50 4–5x lower when busy at 30% less batch (section 6.12) |
| E2s-n | 2026-10-30 | a2ac442 | eevdf | `e2s-eevdf-normal.conf`: as E2s with zstd SCHED_OTHER | 5 | `results/2026-10-30-e2s-eevdf-normal/` | Same as eevdf with zstd SCHED_BATCH in E2s; the S1p/S2p failures were noise (section 6.12) |
| MLtr | 2026-10-30 | 02cb689 | split (reactive) with `--ml-trace` | `ml-train.conf`, `ML_SEED` 1–3: `ml/patterns.py random` 60 s | 1 each | `results/2026-10-30-ml-train-s{1,2,3}/` | Training data, 1,277–1,291 samples each; other processes 16–20% |
| MLte | 2026-10-30 | 549bf7c | split, splitml (`--ml-model ml/models/burst-v1.txt`), splitbuf (`--lc-buffer-pct 150`), batchvicp | `ml-test-short.conf` (`ml/patterns.py short 101 40`), `ml-test-periodic.conf` (E2 pattern) | 5 | `results/2026-10-30-ml-test-short/`, `-periodic/` | The model keeps the LC pool at 3 CPUs ~95% of the time, like a 1.5 CPU buffer; ML ≈ bigger buffer offline and online (section 6.13) |
| E2f | 2026-10-30 | 93d2647 | eevdf, bpfland, lavd (autopilot), lavdperf (`--performance`), never3, always, batchvicp, kairos (`--split --split-cpus 0-2 --class policy --lc-buffer-pct 150`) | `e2f-periodic.conf` (E2 pattern), `e2f-short.conf` (`ml/patterns.py short 101 40`); zstd SCHED_BATCH, GOMAXPROCS 16; start delayed 90 s | 5 | `results/2026-10-30-e2f-periodic/`, `-short/` | No failed requests. Short rep 5 left out (host slowdown in its last three runs, section 6.14). kairos has the lowest busy p50 (0.9–1.0 ms; bpfland 1.4, eevdf 5.2–5.5, lavd 6.7–7.9) and fewest busy requests over 2/5/10 ms; bpfland has 7–9% (quiet) / 13–28% (busy) more batch MB/s (section 6.14) |
| E2k | 2026-10-31 | 7f1a122 | bpfland, kairos0 (E2f's kairos, `--no-kick-idle`), kairos (idle kicks), kairosnp (kicks, `--no-borrow-preempt --borrow-slice-us 1000`) | `e2k-periodic.conf`, `e2k-short.conf`: as E2f | 5 | `results/2026-10-31-e2k-periodic/`, `-short/` | **Invalid.** The host held the VM back: calibration 689 / 529 iterations/µs instead of ~1035 (requests carried half to two thirds of the intended work), VM time share 0.02–0.93 in all periodic runs and short reps 1–3 (section 6.15). To be rerun |
| E2k-r | 2026-11-01 | 11f6a01 | as E2k | as E2k; Mac plugged in with `caffeinate`, IDE idle, start delayed 60 s | 5 | `results/2026-11-01-e2k-periodic/`, `-short/` | Clean: calibration 1026–1036, VM time share 0.97–0.98 in all 40 runs, other processes mostly ~5%. Idle kicks: no gain. Borrower preemption is what gives KairOS its latency; without it KairOS is worse than bpfland (section 6.16) |
| E2a | 2026-11-01 | e735eae | eevdf, bpfland, lavd, lavdperf, never3, kairos (as E2f) | `e2a-azure.conf`: Azure Functions 2021 trace, hours 36, 54, 284 (random, seed 1), 100x compressed, per-hour peak 3500 req/s, floor 300 (`workloads/traces/azure_phases.py`); otherwise as E2f | 5 | `results/2026-11-01-e2a-azure/` | No failed requests; calibration 1034. Reps 1 and 4 partly disturbed (VS Code and this session, 20–27% other CPU in 3 runs); leaving them out changes nothing. kairos: lowest p50 at every load level and fewest requests over 2/5/10 ms in every rep; 8% less batch than bpfland, 11% less than never3 (section 6.17) |
| E3 | 2026-11-02 | a06db56 | eevdf, bpfland, lavd, never3, kairos (as E2a, without lavdperf) | `e3-burst.conf`: busy (3500 req/s) and quiet (500) phases of equal length L, 50% duty, one block per L = 10, 30, 100, 300, 1000, 3000, 10000 ms in shuffled order (seed 1), 2 s quiet before each block (`workloads/traces/burst_sweep.py`); otherwise as E2f | 5 | `results/2026-11-02-e3-burst/` | **Invalid.** The VM was short of memory and swapping: seconds with ≥ 10% of requests over 50 ms were 10–38% of each config's run time, against 0–6% in E2k-r and E2a (section 6.18). To be rerun |
| E3-md | 2026-11-02 | a06db56 | eevdf | as E3, 1 s samples of /proc/vmstat (`vmstat_1s.txt`, `vmsample.sh` in the run directory) | 1 | `results/2026-11-02-e3-memdiag/` | Diagnostic. The three worst seconds (58–95% of requests over 50 ms) came with 190–940 pages swapped in per second (section 6.18) |
| E3-r | 2026-11-02 | 91c348a (run.sh at abdb4ec; `git_dirty` true, section 6.19) | as E3 | as E3; page cache dropped before the batch and after each run (`scripts/evict_cache.py`); 1 s memory counters | 5 | `results/2026-11-02-e3-burst-171449/` | Clean apart from rep 1 (lavd-r1 swapped 34k pages; bpfland-r1 VM share 0.93); no reclaim stalls. kairos busy p50 0.90–0.98 ms at every L, lower than every baseline in 5/5 paired; 6–20% less batch MB/s (section 6.19) |
| E7 | 2026-11-02 | 91c348a | eevdf, simple, bpfland, lavd, never3, kairos (bpfstats without eevdf) | `e7-batch.conf` (3 zstd, 10 req/s), `e7-light.conf` (500 req/s, no batch), `experiments/e7_pipe.sh` (perf bench sched pipe, 1 and 2 CPUs), `e7-bpfstats.conf` (kernel.bpf_stats_enabled=1) | 5 (bpfstats 2) | `results/2026-11-02-e7-{batch,light,pipe,bpfstats}/` | Batch only: all within ±3% of simple (rep 1 left out, disturbed by my analysis). Light load: kairos lowest p50 (1.04 ms). Pipe on 2 CPUs: kairos 15.2 µs per round trip vs simple 1.6 (ignores sync wakeups). Callbacks 4.1% of a CPU (simple 1.5, bpfland 5.5, lavd 7.3) (section 6.20) |
| E3-idle | 2026-11-02 | 8086c1f | eevdfidle (Linux, zstd SCHED_IDLE), kairos (zstd SCHED_BATCH, as E3) | `e3-idle.conf`: E3's phases | 3 | `results/2026-11-02-e3-idle/` | All clean. kairos busy p50 0.89-1.05 ms vs 1.8-3.0 for eevdfidle, better in 3/3 at every L; eevdfidle 2% more batch over the whole run (section 6.21) |
| E10 | 2026-11-04 | 98f977e | kairos (as E3), kpw (`--preempt-wait-us=250 --preempt-min-run-us=100`); `--diag` with groups grpcworker, grpcserver, zstd | `e10-preempt-diag.conf`: E3's phases | 3 | `results/2026-11-04-e10-diag/` (a first start on 2026-11-03 was stopped after one run because the IDE was in use: `results/2026-11-03-e10-diag-aborted/`) | Clean apart from steady direct-reclaim and compaction stalls (no swapping). 94–96% of kairos's preempting runs are the server's non-worker threads, mean 7–12 µs; the request workers 2–5%, ~0.5 ms. kpw cut preemptions from 8,740 to 1,384/s (section 6.22) |
| E11 | 2026-11-04 | 98f977e | kairos, pw100r100 / pw250r100 / pw1000r100 (`--preempt-wait-us` 100 / 250 / 1000, `--preempt-min-run-us=100`), pw250 (D 250 µs, every LC task waits), bpfland, eevdfidle | `e11-delay.conf`: E3's phases | 5 | `results/2026-11-04-e11-delay/` | Clean (VM share 0.98–1.00, other processes ≤ 7%); fewer reclaim stalls than E10. pw1000r100: 90% fewer preemptions, 6% more batch than kairos (5/5 reps) and as much as bpfland (399 vs 394 MB/s); busy p50 1.21–1.45 ms vs 0.94–1.10 (kairos) and 1.55–1.69 (bpfland, lower in 4–5/5 at every L) (section 6.23) |
| E12 | 2026-11-04 | 98f977e | kairos, sync (`--wake-sync`), percpu (`--lc-queue percpu`), longest / prev (`--borrower-pick`), eevdfidle | `e12-ablate.conf`: E3's phases | 3 | `results/2026-11-04-e12-ablate/` | Clean. Per-CPU LC queues raise busy p50 from 0.86–1.04 to 1.46–1.71 ms (eevdfidle 1.99–2.99): the shared queue is a large part of kairos's lead. sync, longest, prev: no clear change (section 6.24) |
| E12-pipe | 2026-11-04 | 98f977e | simple, never3, kairos, kairossync (`--wake-sync`) | `experiments/e7_pipe.sh` with `ORDER`, `NAME=e12-pipe` | 5 | `results/2026-11-04-e12-pipe/` | 2 CPUs: kairossync 1.58 µs per round trip vs kairos 14.86, simple 1.45 (section 6.24) |
| G | 2026-11-04 | 98f977e | eevdf, eevdfidle, bpfland, kairos, pw50, pw200 (`--preempt-wait-us` 50 / 200) | `g-redis.conf`: 3 redis-server, GETs of random 1 KB values, 5 s at 2,000 and 20,000 req/s ×6, 3 zstd | 5 | `results/2026-11-04-g-redis/` | Clean. Redis p50 0.1 ms under kairos vs 0.5 under Linux (both batch policies) and bpfland, at 6% less batch than eevdf. p99s are set by the load generator (send lag p99 1–8 ms) (section 6.25) |
| G-nb | 2026-11-04 | 47801f7 | eevdf, bpfland, kairos | `g-redis-nobatch.conf`: G without batch | 3 | `results/2026-11-04-g-redis-nobatch/` | Clean. p50 0.1–0.2 ms in all three, so G's p50 gap comes from sharing CPUs 0–2 with batch (section 6.25) |
| FS | 2026-11-04 | 3b943c9 + uncommitted final configs (committed as 671776a) | eevdf, eevdfidle, bpfland, lavd, lavdperf, never3, kairos, pw1000r100 (as `final-burst.conf`) | `final-burst.conf` with a 12 s sweep (`burst_sweep.py --lengths-ms 100,1000 --block-s 4`), cooldown 3 s; VM up 5 min | 1 | `results/2026-11-04-final-smoke/` | Smoke test, not a measurement. All 8 configs load (lavd prints libbpf's "skeleton link is uninitialized" warning, as in every E3-r lavd run); eevdfidle's zstd runs SCHED_IDLE; stats.py and compare.py run on it |
| AP | 2026-11-04 | 1018388 | offline, no scheduler | `ml/azure_predict.py`: Azure trace at 1 s bins (10x), depth-5 tree vs simple predictors of the next 200 ms's LC demand, pools scored at equal cost; test hours 36, 54, 284 and 12 others | – | `results/2026-11-04-azure-predict/` | The tree is under-provisioned less often than the best simple predictor in 0/3 of E2a's hours and 2/12 others (by ≤ 0.3 points), so the condition for a run with more CPUs is not met (section 6.27) |
| DS | 2026-11-04 | 3067b18 | kairos, pw1000r100, fixed3 (kairos with `--lc-min=3`) | `final-burst.conf` with 1 rep | 1 | `results/2026-11-04-dup-smoke/` | Not a measurement: the host slowed the VM in all three runs (server CPU per request 910–1,114 µs vs 674–677 in FS and E11; load generator 190–222 vs 137–148 µs; VM share 0.94–0.95); kairos busy p50 5.1 ms, batch 160 MB/s. The new counters work: dup_alt 8.9% of kairos's preemptions, 3.0% of pw1000r100's (section 6.28) |
| DS2 | 2026-11-04 | 60c15d8 | kairos, pw1000r100, fixed3 | DS rerun, Mac idle (`caffeinate`) | 1 | `results/2026-11-04-dup-smoke2/` | Clean (VM share 0.98, server 643–671 µs per request). kairos dup_alt 8.8% of 8,061 preemptions/s, pw1000r100 0.8%, so the fix is tested first: E14 (section 6.29) |
| E14 | 2026-11-04 | e11c590 | kairos, kclaim, pw1000r100, pwclaim (`--claim-borrower`) | `claim-burst.conf` (E3's phases), `claim-azure.conf` (E2a's replay); `experiments/e14_claim.sh` | 10 | `results/2026-11-04-claim-burst/`, `results/2026-11-04-claim-azure/` | All 80 runs at VM share 0.98. Flagged (swap-ins > 1,000): claim-burst kairos-r1 (1,593), claim-azure pw1000r100-r7 (1,874); their pairs were dropped. kclaim: busy p50 −2% (p = 0.008), nothing worse. pwclaim: no primary metric differs at p < 0.05. Adopted for both by the rule in 5.1 (section 6.30) |
| GMP3 | 2026-11-04 | 6fefc35 | eevdf, bpfland, kairos, pw1000r100 (both with `--claim-borrower`) | `gmp3-burst.conf`: E3's phases with the server at GOMAXPROCS 3 | 6 | `results/2026-11-04-gmp3-burst/` | Clean (VM share 0.98–1.00, none flagged). kairos's ordering holds: in 6/6 reps the lowest busy p50 (0.82 ms; pw1000r100 1.13, bpfland 1.56) and the least batch (411 MB/s; 429, 443). EEVDF is overloaded: busy p50 227 ms, growing with burst length. Holm can't reject at n = 6 with 3 comparisons (section 6.31) |
| F | 2026-11-05 | b10fe6d | eevdf, eevdfidle, bpfland, lavd, lavdperf, never3, kairos, pw1000r100, and fixed3 in the burst part only; kairos, pw1000r100 and fixed3 with `--claim-borrower` | `final-burst.conf` (E3's phases), `final-azure.conf` (E2a's replay); `experiments/final.sh`, 02:30–09:25, apt timers stopped | 10 | `results/2026-11-05-final-burst/`, `results/2026-11-05-final-azure/` | No run flagged. pw1000r100 is the main configuration by the rule in 5.1. Every run also had my wait process using 3.5–8% of a CPU, and the server's CPU per request was ~10% above E14's all night (section 8). Section 6.32 |

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
| E10 | Which wakeups preempt (`e10-preempt-diag.conf`, `--diag`) | E3's phases | which threads preempt, how long they then run |
| E11 | Delayed borrower preemption (`e11-delay.conf`) | E3's phases | latency for batch throughput as the wait D grows |
| E12 | Ablations (`e12-ablate.conf`, `e7_pipe.sh` with kairossync) | E3's phases; pipe | sync wakeups, shared vs per-CPU LC queue, borrower choice |
| F | Final evaluation (`final-burst.conf`, `final-azure.conf`, `experiments/final.sh`) | E3's phases; E2a's Azure replay | every baseline and both kairos variants (D = 0 and pw1000r100), 10 reps, tested with `analysis/stats.py`; in the burst part also fixed3 (LC pool held at 3 CPUs), a pool-sizing ablation; after E14, kairos, pw1000r100 and fixed3 run with `--claim-borrower` |
| E13 | Dependency rule for delayed preemption | E3's phases | whether letting short-running LC tasks that recently woke another LC task preempt at once recovers pw1000r100's latency at its batch throughput |
| E14 | One borrower preemption per CPU at a time (`claim-burst.conf`, `claim-azure.conf`) | E3's phases; E2a's Azure replay | whether `--claim-borrower` (a second LC wakeup takes another borrower or queues, instead of re-picking a CPU already being preempted) changes latency or batch throughput, for kairos and pw1000r100 |

**Rules for F, set on 2026-11-04 before F runs** (Aayush agreed to the
draft):
- **Run checks.** summarize_run.py and compare.py report each run's VM
  time share, other processes' CPU, pages swapped in and direct-reclaim
  stalls. A run is flagged if any of these holds:
  - VM share < 0.95;
  - other processes > 30% of a CPU;
  - more than 1,000 pages swapped in;
  - more than 1,000 direct-reclaim stalls.

  The thresholds are first proposals from sections 6.14–6.25, set before
  F's data exist. run.sh already refuses to start below 950
  iterations/µs.
- **Exclusion.** A flagged run's rep is left out for every config
  (`stats.py --skip-reps`), so every comparison stays paired.
  - Open, to decide before F runs: whether to drop only the flagged
    run's pairs instead.
  - Reason: with two reps left out (n = 8), Holm can't reject anything.
    The smallest exact p is then 2/256 = 0.0078, above 0.05/8 (the burst
    part, 8 comparisons per reference) and 0.05/7 (the Azure part).
- **Primary metrics.**
  - Burst sweep: busy p50 (all L), busy share over 5 ms (all L),
    whole-run batch MB/s.
  - Azure replay: p50, share over 5 ms and batch MB/s over all requests.
  - Every other metric (per L, per level, p99, p99.9, quiet) is
    descriptive.
- **Tests.** `stats.py --ref kairos pw1000r100`: exact Wilcoxon
  signed-rank paired by rep, Holm within each metric and reference,
  α = 0.05, bootstrap 95% CIs of the median paired difference.
- **Main configuration.** pw1000r100 is the main configuration if, on the
  burst sweep against bpfland, its busy p50 is lower with Holm p < 0.05
  and its whole-run batch MB/s is not lower with Holm p < 0.05.
  Otherwise kairos is. The Azure replay is reported alongside.
- **Duplicate picks (DS).** If `dup_alt` is under 1% of kairos's
  preemptions on a clean run, F runs with the code as it is. Otherwise a
  fix is tested on its own first: a per-CPU reservation, so that a second
  LC wakeup takes another borrower or waits in LC_DSQ.
- **E13 success** (proposed, for Aayush to confirm). The dependency
  variant beats pw1000r100 if both hold:
  - its busy p50 (all L) is lower with Holm p < 0.05;
  - its median paired whole-run batch MB/s is within 2% of
    pw1000r100's.

  Fewer preemptions alone is not success.

**Added on 2026-11-04 (evening), before E14 and F run:**
- **Exclusion, decided** (Aayush chose this over the whole-rep rule
  above). Only the flagged run's pairs are left out: `stats.py
  --skip-runs <config>-r<rep>`. If the flagged run is a reference (kairos
  or pw1000r100), that rep drops out of every comparison with that
  reference. A flagged run of another config costs one pair, so the other
  comparisons keep n = 10.
- **Duplicate picks, outcome.** In DS2, a clean run, kairos's dup_alt was
  8.8% of its preemptions (section 6.28). That is above the 1%, so the fix
  (`--claim-borrower`) is tested on its own first, in E14.
- **E14 adoption rule.** `--claim-borrower` is adopted for kairos (and
  separately for pw1000r100) if, against the same config without it, it
  is not worse with p < 0.05 on any of the three primary metrics:
  - on the burst sweep: busy p50, busy share over 5 ms, whole-run batch
    MB/s;
  - on the Azure replay: p50, share over 5 ms, batch MB/s.

  Each pair is its own decision: `stats.py --ref kairos --configs kairos
  kclaim`, one comparison per metric, so no Holm. Runs are checked with
  F's flags.

  Reason: the option removes a known misbehaviour, so it is kept unless
  it costs something measurable. Whether it also helps is reported
  either way. An adopted fix then runs in F under the same config names,
  with `--claim-borrower` added; F's config.conf records it.

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

### 6.8 E2 pilots: adaptive switching (2026-10-30)
Single runs while the IDE was in use (other processes 20–100% of a CPU),
so only the mode behaviour counts, not latency differences. 5 s phases,
500 / 3500 req/s alternating, GOMAXPROCS 16, signal CPUs 0–2.
- 2 zstd (a025a0d + 3bd9dff): adaptive was in LATENCY ~1000 ms of every
  second. Each try at BATCH sent the late share to 47–64% and back.
  Per second in LATENCY, 0.7–2% of wakeups were late at 500 req/s and
  4–24% at 3500.
- 1 zstd, late share smoothed over ~8 ms: per-second late share in quiet
  phases 0.1–0.9%, yet each BATCH try ended within a second.
- 1 zstd, smoothed over ~64 ms, x1024 fixed point (a7fd0f6): the EWMA sat
  at 6.2% with 0.1% late per second; never left LATENCY.
- 1 zstd, 2^20 fixed point (6206245, `results/2026-10-30-e2-pilot-z1-021630/`):
  LATENCY 45–156 ms per second in the first quiet phase, 960–1000 ms per
  second in busy phases, back to BATCH in quiet phases but sometimes only
  after 2–3 s (the hold had doubled during the busy phase). Batch MB/s in
  quiet phases: adaptive 314–319, batchvic 282–321, never 293–310.

### 6.9 E2: bursty load (2026-10-30)
5 s phases, 500 (quiet) and 3500 (busy) req/s alternating three times;
GOMAXPROCS 16; 10 interleaved reps; IDE idle. Medians over reps, per
phase in order (quiet phases 0/2/4, busy phases 1/3/5); ms unless noted.
No failed requests. "LAT ms/s" = time in LATENCY mode per second.

1 zstd job (`results/2026-10-30-e2-z1/`, adaptive target 1 ms):

| config | quiet p99 | quiet batch MB/s | busy p50 | busy p99 | busy batch MB/s | LAT ms/s quiet; busy |
|---|---|---|---|---|---|---|
| eevdf | 10 / 3 / 3 | 314 / 329 / 329 | 3.9 / 3.1 / 2.4 | 44 / 32 / 25 | 164 / 179 / 179 | – |
| never | 3 / 5 / 3 | 333 / 333 / 333 | 5.7 / 4.6 / 5.0 | 80 / 79 / 82 | 253 / 256 / 243 | – |
| never3 | 4 / 3 / 4 | 334 / 333 / 329 | 2.9 / 2.7 / 2.7 | 23 / 18 / 22 | 228 / 236 / 223 | – |
| batchvic | 3 / 3 / 3 | 324 / 322 / 327 | 1.4 / 1.2 / 1.2 | 108 / 12 / 11 | 166 / 180 / 172 | – |
| adaptive | 4 / 7 / 5 | 332 / 328 / 326 | 1.3 / 1.3 / 1.3 | 14 / 15 / 14 | 174 / 174 / 164 | 0 / 321 / 624; 982 / 992 / 997 |

- Quiet p50 0.8 for every kairos config, 0.9 for eevdf.
- adaptive's quiet batch MB/s above batchvic in 8/7/5 of 10 reps; busy
  p99 below batchvic in 9/3/4 of 10.
- batchvic's phase-1 p99 (108) comes from 1 s windows with p99 170–440 ms
  in 6/10 reps, at different times in the phase. In batchvic-r5 the
  window overlapped GC cycles stretched to 20–37 ms wall (`episodes.py`).
  adaptive's worst 1 s window in phase 1: 87 ms.

2 zstd jobs (`results/2026-10-30-e2-z2/`; a5, a10 = adaptive with 5 / 10
ms targets):

| config | quiet p99 | quiet batch MB/s | busy p50 | busy p99 | busy batch MB/s | LAT ms/s quiet; busy |
|---|---|---|---|---|---|---|
| eevdf | 5 / 5 / 5 | 584 / 590 / 581 | 6.3 / 5.0 / 4.4 | 68 / 61 / 68 | 223 / 230 / 222 | – |
| never | 7 / 10 / 16 | 641 / 628 / 612 | 10.3 / 9.1 / 9.7 | 131 / 141 / 154 | 299 / 285 / 261 | – |
| never3 | 6 / 7 / 6 | 627 / 594 / 611 | 5.6 / 4.3 / 3.9 | 53 / 38 / 35 | 265 / 265 / 259 | – |
| batchvic | 5 / 5 / 7 | 596 / 588 / 549 | 1.9 / 1.8 / 2.2 | 34 / 25 / 32 | 221 / 224 / 191 | – |
| a5 | 18 / 17 / 13 | 637 / 608 / 600 | 5.2 / 3.9 / 2.0 | 118 / 67 / 58 | 226 / 257 / 222 | 0 / 114 / 477; 752 / 777 / 1000 |
| a10 | 27 / 12 / 11 | 612 / 623 / 614 | 11.0 / 8.6 / 7.8 | 154 / 184 / 127 | 285 / 278 / 272 | 0 in every phase |

- Quiet p50: batchvic 0.9–1.0, others 1.0–1.2.
- a10's late share (wakeups on CPUs 0–2 waiting > 10 ms) was ≤ 0.4% in
  busy phases while request p99 was 127–184 ms.
- a5's quiet batch MB/s above batchvic in 10/7/9 of 10 reps; busy p99
  below batchvic in 1/4/5 and below never3 in 3/4/5 of 10.
- Runs with other processes above 30% of a CPU: z1 eevdf-r2 (45%), z2
  batchvic-r1 (72%, mostly the VS Code server) and never3-r4 (31%).

E2pt pilot (`results/2026-10-30-e2-pilot-target/`, one run each, 2 zstd):
a10 stayed in BATCH all run; a5 was mostly in LATENCY, its BATCH tries
ending within 1–2 s (the per-second samples don't show the late-share
spike that ended each one).


### 6.10 E2 diagnostics: which waits the signal misses (2026-10-30)
ED (`results/2026-10-30-e2-diag/`, 3 reps), busy phases (3500 req/s),
pooled over reps. Wait time per completed request, by group and kind:

| config | worker wait ms/req (wakeup / requeue) | other server threads | Go waits ≥ 5 ms / ≥ 20 ms | mean latency |
|---|---|---|---|---|
| never | 2.10 / 0.03 | 0.62 / 0.01 | 5.1% / 2.0% | 78.1 |
| a10 | 1.91 / 0.01 | 0.67 / 0.00 | 2.2% / 0.23% | 20.0 |
| a5 | 1.08 / 0.01 | 0.71 / 0.01 | 1.4% / 0.35% | 19.2 |
| never3 | 0.94 / 0.03 | 1.08 / 0.01 | 0.33% / 0.02% | 7.5 |
| batchvic | 0.47 / 0.01 | 0.79 / 0.03 | 0.27% / 0.08% | 8.2 |
| eevdf | – | – | 0.42% / 0.11% | 23.0 |

(requeue = preempted + slice ran out; Go waits = share of sampled
goroutine waits; mean latency in ms, inflated by tail episodes.)

- Requeue waits are ≤ 4.2% of the server threads' waits and ≤ 3.7% of
  their waiting time in every config: the server's threads sleep before
  their slice ends.
- Worker wakeup waits (busy): never 2–5 ms 59%, 5–10 ms 6.1%, ≥ 10 ms
  1.2%; a10 55.6 / 5.5 / 0.3%; a5 27.9 / 1.9 / 0.4%; never3 21.4 / 0.4 /
  0%; batchvic 7.9 / 0.3 / 0.1%. Other server threads' wakeups ≥ 5 ms:
  never 1.5%, batchvic 0%.
- Wakeups per request (busy): workers 0.8–0.9, other server threads
  1.6–1.9 (never, a10) to 4.6 (batchvic), load generator 3.1–3.9.
- Server threads' CPU (busy): `grpcserver` threads 14–15% of a CPU under
  never/a10, 38.5% under batchvic; workers 179–196%.
- zstd in busy phases: preempted 0.5 (a5) and 1.0 (batchvic) times per
  request (~1,800 and ~3,600/s); 0 under never and a10.

ES (`results/2026-10-30-e2-split/`, 2 reps), busy, ms, medians over reps
of p50; then the parts' mean over each rep's slowest 1%:

| config | p50 | in | queue | service | wake | out | slowest 1%: in + out share |
|---|---|---|---|---|---|---|---|
| never | 8.2 | 1.32 | 2.30 | 0.52 | 0.01 | 0.21 | 87% of 852 |
| a10 | 9.9 | 1.68 | 2.29 | 0.52 | 0.01 | 0.22 | 97% of 546 |
| a5 | 2.2 | 0.35 | 0.43 | 0.52 | 0.01 | 0.11 | 85% of 747 |
| never3 | 4.4 | 0.74 | 0.72 | 0.52 | 0.03 | 0.13 | 88% of 374 |
| batchvic | 1.7 | 0.28 | 0.09 | 0.52 | 0.02 | 0.10 | 81% of 722 |
| eevdf | 6.6 | 1.22 | 1.31 | 0.52 | 0.08 | 0.20 | 85% of 970 |

- Quiet phases, never, slowest 1% (117 ms): lag 32 + in 45 + queue 12 +
  service 2 + wake 11 + out 15.
- Tails in ED, ES and EC were 2–8x E2's for every config, Linux's
  included. EC (`results/2026-10-30-e2-control/`, diagnostics off) busy
  p99: batchvic 30–189, eevdf 60–323, never 79–1476 ms (E2: 25–34,
  61–68, 131–154). Slow requests were spread over the whole second
  after each `schedlat` read (in ES the first 100 ms had 1.4x the
  average, in ED no excess), and GC counts and durations were close to
  E2's. So the counters didn't cause it; these evening runs were noisier
  (cause not identified; the host is the guess). Medians are close to
  E2's; tail numbers from ED/ES/EC are not comparable with E2.


### 6.11 Soft CPU split pilots (2026-10-30)
Tentative: other processes used 3–137% of a CPU per run (VS Code server,
the Claude CLI, kswapd under memory pressure; 373 MB swap in use), which
moves tails by hundreds of ms. Medians over reps, ms.

S2p, GOMAXPROCS 16, 2 reps (phases 1/3/5 busy, 0/2/4 quiet):

| config | quiet p50 | quiet p99 | quiet batch MB/s | busy p50 | busy p99 | busy batch MB/s | other CPU % |
|---|---|---|---|---|---|---|---|
| never3 | 1.2–1.3 | 40–90 | 492–531 | 7.5–69 | 429–781 | 134–210 | 26 |
| batchvicp | 1.0–2.6 | 33–471 | 399–410 | 1.6–2.9 | 309–741 | 142–155 | 28 |
| split | 0.8–0.9 | 8–27 | 495–547 | 1.0–1.3 | 35–122 | 164–192 | 15.5 |
| splitnp | 1.1–3.1 | 28–43 | 361–532 | 2.6–8.2 | 285–465 | 132–210 | 45 |
| eevdf | failed requests in both reps | | | | | | 75 |

- Busy-phase waits per request (diag): workers split 0.47, batchvicp
  0.90, never3 1.63, splitnp 1.31 ms; Go goroutine waits ≥ 20 ms 0.06,
  1.25, 2.17, 0.56%. zstd preempted 1.6–1.7 times per request under split
  and batchvicp (~5,500–6,000/s), 0 under never3. Server non-worker
  threads' CPU: never3 24, batchvicp 32, split 33, splitnp 27% of a CPU.
- Split's LC pool: 1–2 CPUs in quiet phases, 3 in busy phases (demand
  2–20 CPUs counting queued LC tasks).
- S2p, GOMAXPROCS 3: busy p50/p99/batch never3 2.3–2.9 / 59–102 /
  278–298; batchvicp 0.8–1.1 / 47–154 / 211–249; splitnp 1.9–31.5 /
  103–186 / 192–273; split collapsed in rep 1 (other processes: VS Code
  server 53%, Claude CLI 24%). Server threads' kernel waits were ≤ 0.04
  ms per request in every config; queueing moved into Go (goroutine
  waits ≥ 20 ms 0.4–8.8%).
- eevdf with zstd as SCHED_BATCH failed requests (timeouts) in all 4 runs
  (both GOMAXPROCS settings) while zstd got 226–376 MB/s in busy phases;
  with zstd as SCHED_OTHER it never failed (E2, EC). Cause not found
  (see section 4: SCHED_BATCH only stops batch wakees preempting).
- S1p (1 rep, GOMAXPROCS 16, no diag): split collapsed in busy phase 1
  (p50 281 ms) and was best in busy phase 5 (p50 1.0, p99 11); batchvicp
  collapsed in busy phases 1 and 3.


### 6.12 E2s: soft CPU split, clean run (2026-10-30)
`results/2026-10-30-e2s/`, 5 interleaved reps, other processes 9–21% of a
CPU, no failed requests. Medians over reps, ms; phases 0/2/4 quiet (500
req/s), 1/3/5 busy (3500 req/s).

| config | quiet p50 | quiet p99 | busy p50 | busy p99 | batch MB/s quiet / busy | preempt/s quiet / busy |
|---|---|---|---|---|---|---|
| eevdf | 1.1–1.2 | 5–33 | 4.6–5.9 | 51–95 | 606 / 238 | – |
| never3 | 1.0 | 7–11 | 3.9–5.0 | 28–35 | 627 / 279 | 0 / 0 |
| batchvicp | 0.8 | 4–8 | 0.9–1.0 | 14–18 | 569 / 191 | ~13,100 / ~13,200 |
| split | 0.8 | 3–5 | 1.0 | 18–36 | 593 / 196 | ~5,000 / ~10,100 |

(batch MB/s: median over reps of the mean over the three phases.)

- split vs batchvicp, reps of 5 where split was better, phases 0–5:
  p50 2/3/2/1/2/3, p99 3/3/2/1/4/3, batch MB/s 3/3/5/2/4/3. A tie on
  latency; batch slightly higher, mostly in quiet phases.
- split vs never3: p50 lower in 5/5 reps in every phase; p99 lower in
  2–4/5; batch MB/s higher in 0–1/5 (quiet −5%, busy −30%).
- E2s-n (`results/2026-10-30-e2s-eevdf-normal/`, eevdf, zstd as
  SCHED_OTHER, run right after): quiet p50 1.1–1.2, busy p50 4.5–5.6,
  busy p99 36–50, batch 601–614 / 230–250 MB/s: as eevdf with zstd as
  SCHED_BATCH. Correction to 6.11: the eevdf failures in S1p/S2p came with
  the noise, not with SCHED_BATCH.


### 6.13 ML burst prediction for the split (2026-10-30)
Offline (`ml/train_burst.py`, tree depth 5, K 10, H 4, buffer 0.5 CPU, 3
CPUs). Trained on random-pattern traces s1, s2; tested on s3:

| predictor | under-provisioned | extra LC CPUs |
|---|---|---|
| persistence | 21.7% | 0.04 |
| ewma (reactive) | 17.3% | 0.05 |
| tree | 2.2% | 0.32 |
| max(ewma, tree) | 1.9% | 0.33 |
| persistence + 1.1 CPU buffer | 1.0% | 0.33 |
| ewma + 1.0 CPU buffer | 1.0% | 0.33 |

Depths 3, 4, 6 gave the same picture. Model burst-v1 (trained on s1-s3,
45 nodes) on the 10 reactive split traces of the test patterns: tree
under 1.3% [0.6–2.4] / over 0.41; ewma + matched buffer 1.4% [0.6–3.4] /
0.44; persistence + matched buffer 1.1% [0.6–2.7] / 0.42.

Online (MLte, 5 reps, other processes 12–18% median; `analysis/byrate.py`,
busy = ≥ 2000 req/s, onset = first 250 ms of a busy phase; medians over
reps, ms and MB/s):

| pattern | config | quiet p99 | busy p99 | onset p99 | batch quiet / busy | LC CPUs |
|---|---|---|---|---|---|---|
| short | split | 13.0 | 179 | 26 | 545 / 298 | 2.43 |
| short | splitml | 6.4 | 43 | 39 | 558 / 313 | 3.01 |
| short | splitbuf | 3.6 | 31 | 8.3 | 569 / 309 | 3.00 |
| short | batchvicp | 3.0 | 45 | 63 | 552 / 306 | – |
| periodic | split | 39.4 | 10.7 | 7.4 | 587 / 214 | 2.55 |
| periodic | splitml | 3.3 | 78 | 14.5 | 589 / 209 | 3.01 |
| periodic | splitbuf | 3.9 | 25 | 6.7 | 595 / 204 | 3.01 |
| periodic | batchvicp | 9.5 | 15 | 10.1 | 560 / 202 | – |

- Quiet p50 0.77–0.83 and busy p50 0.86–0.95 for every config.
- The model raised the demand in 94–97% of the ticks its prediction was
  fresh.
- Busy p99 per rep swings from 5–10 to 170–2,400 ms within a config
  (slow episodes in some reps of every config), so busy and onset tails
  don't rank the configs. Quiet p99 is steadier: the reactive split's
  small pool gave 36–49 ms in 4/5 periodic reps, splitml and splitbuf
  3–9 ms.
- splitbuf vs batchvicp: quiet batch MB/s higher in 5/5 (short) and 4/5
  (periodic) reps; latency counts mixed (1–5/5).

---

### 6.14 E2-final: main comparison against the baselines (2026-10-30)
`results/2026-10-30-e2f-periodic/` (5 reps) and `-short/` (reps 1–4;
rep 5 left out, see below). Configs interleaved per rep, zstd
SCHED_BATCH for all, no failed requests. `analysis/byrate.py` (busy =
3500 req/s phases / ≥ 2000 req/s bursts), medians over reps, ms and MB/s;
">2/5/10" = % of busy requests slower than 2/5/10 ms.

Periodic (E2's pattern):

| config | quiet p50 | busy p50 | busy >2/5/10 ms % | busy p95 | busy p99 | batch quiet / busy |
|---|---|---|---|---|---|---|
| eevdf | 1.06 | 5.21 | 82 / 51 / 29 | 139 | 342 | 623 / 252 |
| bpfland | 0.90 | 1.45 | 38 / 14 / 5.7 | 10.8 | 37 | 630 / 243 |
| lavd | 1.00 | 7.02 | 95 / 67 / 38 | 53 | 126 | 633 / 280 |
| lavdperf | 1.03 | 7.86 | 95 / 68 / 43 | 135 | 310 | 616 / 276 |
| never3 | 1.02 | 4.13 | 78 / 42 / 23 | 31 | 147 | 617 / 268 |
| always | 0.88 | 2.27 | 54 / 25 / 9.4 | 69 | 200 | 549 / 140 |
| batchvicp | 0.80 | 1.03 | 25 / 12 / 7.6 | 21 | 154 | 564 / 182 |
| kairos | 0.80 | 0.96 | 18 / 8.9 / 4.7 | 9.4 | 55 | 576 / 190 |

Short bursts (`ml/patterns.py short 101 40`):

| config | quiet p50 | busy p50 | busy >2/5/10 ms % | busy p95 | busy p99 | batch quiet / busy |
|---|---|---|---|---|---|---|
| eevdf | 1.16 | 5.50 | 81 / 53 / 33 | 71 | 153 | 585 / 341 |
| bpfland | 0.97 | 1.40 | 36 / 13 / 4.7 | 9.5 | 44 | 593 / 349 |
| lavd | 1.06 | 6.91 | 94 / 66 / 37 | 47 | 230 | 611 / 390 |
| lavdperf | 1.04 | 6.65 | 94 / 64 / 35 | 49 | 126 | 616 / 386 |
| never3 | 1.03 | 4.12 | 77 / 42 / 19 | 18 | 28 | 617 / 380 |
| always | 0.90 | 1.55 | 42 / 14 / 5.5 | 38 | 151 | 522 / 271 |
| batchvicp | 0.78 | 0.87 | 14 / 6.1 / 4.1 | 7.0 | 180 | 546 / 305 |
| kairos | 0.80 | 0.89 | 14 / 4.7 / 2.4 | 4.8 | 46 | 553 / 310 |

- kairos vs bpfland, reps where bpfland was better (periodic 5, short 4):
  busy p50 0/5, 0/4; busy p99 4/5, 1/4; batch MB/s quiet 5/5, 4/4 and
  busy 5/5, 4/4. bpfland: 9% / 7% more batch when quiet, 28% / 13% more
  when busy. kairos vs eevdf, lavd, lavdperf, never3: busy p50 lower in
  every rep.
- Quiet tails: % of quiet requests over 5 ms kairos 1.9 / 2.0, bpfland
  1.1 / 1.4, batchvicp 0.9 / 1.4, eevdf 0.9 / 2.1 (periodic / short):
  kairos is not better when quiet.
- Busy p99 per rep still swings: over the periodic busy phases kairos
  5–530 ms, bpfland 8–437 ms, batchvicp 6–961 ms.
- Whole run (`analysis/compare.py`), median over reps, periodic / short:

| config | ctx switches k/s | system time % | server %CPU | idle on CPUs 0–2, %CPU | busy preempt/s |
|---|---|---|---|---|---|
| eevdf | 40 / 35 | 9.2 / 9.1 | 124 / 82 | 32 / 43 | – |
| bpfland | 40 / 32 | 7.3 / 6.7 | 130 / 87 | 29 / 41 | – |
| lavd | 26 / 24 | 7.4 / 6.9 | 124 / 83 | 26 / 37 | – |
| lavdperf | 26 / 24 | 7.8 / 7.1 | 127 / 83 | 25 / 37 | – |
| never3 | 29 / 25 | 7.0 / 5.8 | 125 / 82 | 28 / 40 | 0 |
| always | 79 / 63 | 13.7 / 11.5 | 147 / 97 | 33 / 47 | ~42,000–49,000 |
| batchvicp | 53 / 49 | 10.6 / 9.2 | 134 / 89 | 34 / 48 | ~13,300–13,800 |
| kairos | 47 / 41 | 10.0 / 8.5 | 136 / 90 | 35 / 47 | ~9,200–10,100 |

(system time: % of all 4 CPUs; server and idle: % of one CPU; preempt/s
from the periodic busy phases.)
- Short rep 5: always-r5, batchvicp-r5 and kairos-r5 (00:28–00:31, the
  last three runs) had quiet p50 1.0 / 3.6 / 3.1 ms and quiet batch 439 /
  108 / 112 MB/s, while the server needed 1.8x its usual CPU for the same
  requests (161% vs ~90%) and the load generator on its own CPU 1.5x
  (28–30% vs 16–20%). The kernel log shows normal scheduler exits. Left
  out with `--skip-reps 5` for every config, so the paired counts stay
  paired.


### 6.15 E2k: invalid run, and what it showed about the checks (2026-10-31)
`results/2026-10-31-e2k-periodic/`, `-short/`. Not usable as a
comparison:
- Each batch calibrates the per-request work once at its start. Both
  calibrations ran while the host was slowing the VM: 689 and 529
  iterations/µs against 1000–1037 in every main experiment before (only
  the E1p and an E2p pilot were also low, 868 and 881). So the "500 µs"
  requests carried ~250–330 µs of work, a lighter load than E2-final.
- VM time share (VM CPU time from /proc/stat over wall time, section 8):
  0.97–0.98 in E2-final apart from the two short rep-5 runs left out
  (0.89, 0.91). E2k: 0.76–0.93 in every periodic run, 0.02 in
  kairosnp-r4 (30 min of wall time for 38 s of VM time: the Mac asleep),
  0.30–0.79 in short reps 1–3; short reps 4–5 at 0.96–0.98. A VS Code
  server started on the VM mid-run (node, up to 318% of a CPU).
- The two clean short reps (lighter load, so only a hint): idle kicks
  fire ~3,800–4,000 times a second in kairos (~2,900–3,100 in kairosnp),
  so idle CPUs missing queued tasks was common, but kairos's batch MB/s
  was not higher than kairos0's (501 / 348 vs 520 / 370 quiet / busy) and
  its server CPUs were idle about as much (54 vs 56% of a CPU). kairosnp
  had a busy p50 of 1.43 ms against bpfland's 0.83, with similar batch.
  Needs the rerun.

### 6.16 E2k rerun: idle kicks and borrower preemption (2026-11-01)
`results/2026-11-01-e2k-periodic/`, `-short/`, 5 reps each, configs
interleaved per rep. Clean: calibration 1034–1036 (periodic) and
1026–1036 (short) iterations/µs, VM time share 0.97–0.98 in every run,
other processes 4–8% of a CPU (one run 17%, mostly this Claude session).
No failed requests. kairos0 = E2-final's kairos (no kicks), kairos = with
idle kicks, kairosnp = kicks, no borrower preemption, 1 ms borrow slices.
`analysis/byrate.py`, medians over reps, ms and MB/s, columns as in 6.14.

Periodic:

| config | quiet p50 | busy p50 | busy >2/5/10 ms % | busy p95 | busy p99 | batch quiet / busy |
|---|---|---|---|---|---|---|
| bpfland | 0.95 | 1.49 | 39 / 14 / 4.1 | 8.8 | 30 | 617 / 242 |
| kairos0 | 0.81 | 0.91 | 15 / 4.2 / 1.9 | 4.4 | 25 | 588 / 212 |
| kairos | 0.81 | 0.94 | 16 / 4.1 / 1.1 | 4.5 | 11 | 594 / 213 |
| kairosnp | 1.00 | 2.42 | 58 / 23 / 7.3 | 12.3 | 28 | 634 / 260 |

Short bursts:

| config | quiet p50 | busy p50 | busy >2/5/10 ms % | busy p95 | busy p99 | batch quiet / busy |
|---|---|---|---|---|---|---|
| bpfland | 0.96 | 1.42 | 36 / 13 / 3.8 | 8.5 | 32 | 597 / 347 |
| kairos0 | 0.80 | 0.87 | 11 / 2.4 / 0.7 | 3.4 | 8.2 | 561 / 321 |
| kairos | 0.82 | 0.89 | 13 / 3.3 / 1.1 | 3.8 | 11 | 560 / 320 |
| kairosnp | 1.03 | 2.22 | 55 / 22 / 8.3 | 14.2 | 29 | 600 / 359 |

- Paired, reps where bpfland beat kairos0 (periodic / short): busy p50
  0/5, 0/5; busy p99 3/5, 1/5; batch quiet 5/5, 5/5; batch busy 5/5, 4/5.
  bpfland's batch lead: 5% / 6% quiet, 14% / 8% busy (E2-final: 7–9% /
  13–28%). kairos0's busy batch here (212 / 321) is above E2-final's
  kairos (190 / 310) for the same config.
- Kicks (kairos vs kairos0), reps where kairos was better: periodic busy
  p50 2/5, batch quiet 4/5, busy 3/5; short busy p50 0/5, quiet p50 0/5,
  batch quiet 1/5, busy 2/5. Batch within 1%.
- kairosnp vs bpfland: batch 3% / 7% (periodic quiet / busy) and 0% / 3%
  (short) higher; busy p50 1.6x and busy >5 ms 1.6–1.8x bpfland's.
- Whole run (`analysis/compare.py`, sched_stats.csv means), periodic /
  short:

| config | ctx switches k/s | system time % | server %CPU | idle on CPUs 0–2, %CPU | kick/s | borrow/s | preempt/s |
|---|---|---|---|---|---|---|---|
| bpfland | 40 / 33 | 7.1 / 6.6 | 130 / 87 | 29 / 41 | – | – | – |
| kairos0 | 48 / 42 | 9.6 / 8.3 | 131 / 89 | 36 / 49 | 0 | 9,050 / 9,590 | 7,970 / 7,790 |
| kairos | 48 / 42 | 9.7 / 8.7 | 133 / 91 | 35 / 47 | 3,900 / 3,700 | 9,050 / 9,640 | 8,000 / 7,870 |
| kairosnp | 33 / 27 | 6.6 / 6.2 | 127 / 86 | 28 / 39 | 3,140 / 3,000 | 2,820 / 3,280 | 0 |

- Idle per CPU (CPU 0 / 1 / 2, % of the run, median over reps; batch and
  server are both pinned to CPUs 0–2): periodic bpfland 9.8 / 9.8 / 9.4,
  kairos0 15.0 / 11.8 / 8.6, kairos 17.3 / 12.8 / 4.6, kairosnp 9.0 / 9.3
  / 9.3; short bpfland 14.3 / 13.4 / 13.7, kairos0 20.1 / 16.0 / 12.6,
  kairos 23.3 / 17.6 / 6.6, kairosnp 13.4 / 12.8 / 13.0.

### 6.17 E2a: Azure Functions trace replay (2026-11-01)
`results/2026-11-01-e2a-azure/`, 5 reps, configs interleaved per rep,
no failed requests, calibration 1034 iterations/µs.

The pattern comes from `workloads/traces/azure_phases.py`, committed as
`workloads/traces/azure2021-pick3-seed1.phases`:
- Arrival = end_timestamp − duration.
- Three one-hour windows: hours 36, 54 and 284, picked at random (seed 1)
  among the hours with at least the median number of arrivals.
- 10 s of trace becomes a 100 ms phase, so each hour replays in 36 s and
  a run lasts 108 s.
- Each hour is scaled so its busiest 10 s are at 3500 req/s, with a
  300 req/s floor.

Mean rates per hour are 1,014 / 1,148 / 372 req/s. Of the replay time,
0% / 16% / 0% is at 2000 req/s or more. Hour 36 is moderate with one
spike, hour 54 has a sustained busy stretch, and hour 284 is one spike
then the floor.

Requests are pooled by their phase's target rate (`analysis/bylevel.py
--windows 3`). Medians over reps, ms; >5 = % of requests slower than
5 ms. Requests per run: 45,000 below 1000, 32,000 at 1000–2000, 15,000
at 2000 or more.

| config | <1000: p50 / >5 | 1000–2000: p50 / >5 | ≥2000: p50 / >5 | all: p50 / p99 / p99.9 | all >2 / 5 / 10 % | batch MB/s |
|---|---|---|---|---|---|---|
| eevdf | 1.24 / 2.9 | 1.68 / 13.6 | 4.09 / 43 | 1.52 / 49 / 109 | 36 / 13 / 5.9 | 566 |
| bpfland | 0.98 / 2.2 | 1.06 / 3.6 | 1.28 / 11.1 | 1.04 / 15 / 100 | 15 / 3.7 / 1.6 | 568 |
| lavd | 1.14 / 5.6 | 1.58 / 14.5 | 3.98 / 41 | 1.43 / 131 / 511 | 37 / 15 / 6.8 | 565 |
| lavdperf | 1.11 / 5.1 | 1.53 / 10.8 | 3.90 / 41 | 1.39 / 118 / 195 | 35 / 13 / 6.3 | 581 |
| never3 | 1.07 / 3.1 | 1.41 / 8.2 | 2.82 / 26 | 1.32 / 22 / 137 | 29 / 8.8 / 3.2 | 588 |
| kairos | 0.81 / 2.0 | 0.78 / 1.9 | 0.81 / 2.8 | 0.80 / 12 / 107 | 5.4 / 2.1 / 1.3 | 523 |

- Paired against kairos, reps where the baseline was better:
  - p50 at each of the three levels: 0/5 for every baseline.
  - Share of all requests over 5 ms: 0/5 for every baseline.
  - p99 of all requests: bpfland 2/5, eevdf 1/5, never3 1/5, lavd and
    lavdperf 0/5.
  - Batch MB/s: 4/5 for every baseline, 5/5 for never3.
- Batch by hour (36 / 54 / 284):

  | config | MB/s |
  |---|---|
  | bpfland | 567 / 545 / 586 |
  | eevdf | 558 / 531 / 598 |
  | never3 | 589 / 568 / 605 |
  | kairos | 517 / 473 / 581 |

  So kairos is 9% / 13% / 1% below bpfland.
- Share of requests over 5 ms by hour (36 / 54 / 284): bpfland 2.7 / 3.9 /
  3.5, kairos 1.3 / 3.4 / 1.1.
- Whole run (`analysis/compare.py`):

  | config | context switches k/s | server %CPU | idle on CPUs 0–2, %CPU |
  |---|---|---|---|
  | eevdf | 35 | 58 | 45 |
  | bpfland | 28 | 60 | 43 |
  | lavd | 24 | 62 | 36 |
  | lavdperf | 25 | 62 | 37 |
  | never3 | 23 | 58 | 42 |
  | kairos | 39 | 66 | 53 |
- Disturbed runs: bpfland-r1 (VM time share 0.948, other processes 24%,
  this Claude session 11% while it wrote a message), lavd-r1 (27%,
  VS Code's node 19%) and lavdperf-r4 (0.94, 20%). Without reps 1 and 4,
  the medians move little: kairos all p50 0.80, over 5 ms 2.0%, batch
  520; bpfland 1.04, 3.7%, 572. Every paired count keeps its direction.


### 6.18 E3: invalid run, memory pressure in the VM (2026-11-02)
`results/2026-11-02-e3-burst/`, 25 runs. Not usable as a comparison:
- The CPUs were fine. Calibration 995 / 1035 / 1013 iterations/µs (the
  fastest is kept). Uncontended service time p10 517 µs, the same as in
  E2k-r and E2a. VM time share 0.93–0.99, except eevdf-r4 (0.76; other
  processes 86% of a CPU: claude 31, node 23, kswapd0 15) and bpfland-r4
  (0.89, 52%).
- Slow episodes were far more common. Seconds in which at least 10% of
  requests took over 50 ms: bpfland 30%, eevdf 38%, lavd 32%, never3
  14%, kairos 10% of all run seconds. E2k-r periodic (the same two rates,
  5 s phases) had 0–2%, and E2a 1–6%. In the 10 s block, bpfland's busy
  p95 was 246 ms and eevdf's 1,059 ms.
- The VM was swapping. It had been up 4 days 7 h with 576–625 MB of swap
  in use. Since boot, ~503k pages (~2 GB) had been swapped back in.
  Resident: VS Code's node ~750 MB over its processes, this Claude
  session 400 MB, of 3.9 GB.
- Diagnostic run E3-md (eevdf, 1 rep, /proc/vmstat every second): over
  153 s, 3,162 pages swapped in and 3,173 out, 2,765 major faults, 879
  direct-reclaim stalls and 2,211 compaction stalls. 14 of 153 seconds
  had at least 10% of requests over 50 ms. The three worst (seconds
  69–71: 58, 95 and 70%) had 235, 936 and 192 swap-ins. The next worst
  (second 9, 40%) had 538. Not every slow second had swap-ins: seconds
  26–27 (33%, 50%) had 13 and 8.
- Even a 10 s smoke run afterwards had 95 direct-reclaim stalls and 244
  compaction stalls (24 swap-ins).
- A hint only, from the invalid data: kairos's busy p50 stayed at
  0.88–1.04 ms at every burst length and at every time since a burst
  started, while every baseline's rose. Its p95 and p99 rose with the
  episodes too (p95 163 ms in the 1 s block).

### 6.19 E3 rerun: burst-length sweep (2026-11-02)
`results/2026-11-02-e3-burst-171449/`, 25 runs, 17:14–18:24. VM not
restarted (up 4 days); memory freed without root instead: the page cache
was dropped before the batch (posix_fadvise DONTNEED; free 197 -> 1,250
MB) and each run's files after it (`scripts/evict_cache.py`).
- Clean apart from rep 1. No direct-reclaim or compaction stalls in any
  run. Swap-ins 0–651 pages per run, except lavd-r1 (34,374 pages, 22,890
  major faults: its first load paging back in). bpfland-r1 had VM share
  0.93 (others 0.97–1.00). Calibration 1,035 iterations/µs. Other
  processes under 30% of a CPU in every run.
- Recorded commit: 91c348a with `git_dirty` true for 24 of 25 runs (the
  first, at 17:14, recorded abdb4ec). The working tree's run.sh was
  abdb4ec's while the batch ran; 91c348a only adds the BPF_STATS hook,
  inactive here.
- Medians over 5 reps (ms; busy = 3500 req/s phases, quiet = 500 req/s;
  MB/s over the block). Leaving out rep 1 changes no ranking.

| L (ms) | busy p50: kairos / bpfland / eevdf / lavd / never3 | busy >5 ms (%) | quiet >5 ms (%) | MB/s |
|---|---|---|---|---|
| 10 | 0.93 / 1.35 / 2.62 / 3.05 / 3.72 | 2.1 / 8.8 / 25 / 27 / 38 | 1.0 / 1.4 / 10 / 12 / 11 | 391 / 436 / 423 / 450 / 495 |
| 30 | 0.97 / 1.33 / 3.50 / 4.57 / 3.43 | 3.4 / 10.7 / 37 / 45 / 33 | 1.6 / 0.9 / 8.8 / 6.6 / 3.8 | 391 / 444 / 444 / 467 / 459 |
| 100 | 0.98 / 1.47 / 5.81 / 5.81 / 3.74 | 3.1 / 13.5 / 55 / 58 / 38 | 0.9 / 1.1 / 7.1 / 4.2 / 2.6 | 397 / 436 / 412 / 468 / 452 |
| 300 | 0.95 / 1.64 / 11.29 / 10.19 / 4.98 | 5.9 / 17.9 / 71 / 78 / 50 | 1.1 / 1.5 / 4.1 / 3.4 / 2.8 | 393 / 427 / 448 / 481 / 450 |
| 1000 | 0.90 / 1.48 / 3.91 / 4.91 / 2.97 | 3.4 / 15.0 / 43 / 49 / 27 | 1.4 / 2.3 / 1.8 / 4.1 / 1.8 | 396 / 421 / 435 / 444 / 456 |
| 3000 | 0.93 / 1.52 / 3.55 / 4.58 / 3.25 | 3.4 / 20.5 / 37 / 44 / 35 | 1.2 / 1.4 / 1.6 / 1.5 / 1.5 | 397 / 425 / 431 / 468 / 462 |
| 10000 | 0.92 / 1.42 / 4.32 / 5.76 / 3.26 | 4.1 / 12.6 / 45 / 58 / 31 | 1.0 / 2.8 / 1.5 / 4.1 / 1.8 | 397 / 425 / 435 / 458 / 463 |

- Paired over 5 reps: kairos had the lower busy p50 than every baseline
  in 5/5 at every L, and the smaller busy >5 ms share in 5/5, except
  bpfland at 100 ms (3/5). Every baseline had more batch MB/s than kairos
  in 3–5/5 at every L.
- By time since a burst started (busy p50, all L pooled): kairos
  0.90–1.00 ms in every bin from 0–2 ms to 3–10 s; bpfland 1.36–1.57;
  eevdf 2.1 at 0–2 ms rising to 8.1 at 30–100 ms; lavd 2.2 rising to
  7.3; never3 3.0–4.3 throughout.
- After a burst ends (quiet >5 ms share, first 2 ms): eevdf 24%, lavd
  19%, never3 17%, falling to their quiet level within ~100 ms; kairos
  1.8% and bpfland 1.6% from the first bin on.
- Busy p99 doesn't rank configs (as since N1): kairos's ranges from 9.7
  to 52 ms between blocks, bpfland's from 11 to 270 ms.
- Whole run, medians: context switches kairos 46.7k/s, bpfland 38.5k,
  eevdf 39.7k, never3 28.8k, lavd 26.9k. Idle on CPUs 0–2: kairos 0.38
  CPUs, others 0.26–0.31. Server CPU: kairos 1.25 CPUs, bpfland 1.23,
  others 1.16–1.17. Rough zstd CPU (3 − idle − server): kairos 1.37,
  bpfland 1.46, eevdf 1.53, never3 1.55, lavd 1.57; MB/s per zstd CPU is
  within ±4% across configs, so the batch gap is CPU time zstd didn't
  get, not slower zstd.
- KairOS's LC pool (sched_stats.csv): 2.98 CPUs on average, at 3 CPUs in
  97–98% of seconds in every rep; preemptions ~8,500/s on average.

### 6.20 E7: scheduler overhead (2026-11-02)
`results/2026-11-02-e7-{batch,light,pipe,bpfstats}/`, 18:25–18:55, commit
91c348a (clean). Configs eevdf, simple (scx_simple), bpfland, lavd
(autopilot), never3, kairos (as E3); bpfstats without eevdf. No run
flagged by VM share (0.97–1.01) or swapping (≤ 184 pages swapped in),
but two batch-only runs, bpfland-r1 and lavd-r1, had python3 at 89% of a
CPU: my E3 analysis, niced and pinned to the load generator's CPU 3. They
were the two lowest runs (607 and 590 MB/s, others 725–834), so a busy
CPU 3 lowers zstd's throughput on CPUs 0–2 too (guess: the host gives
the vCPU threads less, e.g. moves them to efficiency cores). Rep 1 of
the batch-only runs is left out below. `analysis/overhead.py`.
Median [min-max] over reps; "n/N" = reps better than simple.
- **Batch only** (`e7-batch.conf`: 3 zstd, SCHED_BATCH, 10 req/s; zstd
  MB/s over 15 s; reps 2–5): eevdf 825 [751–834] (3/4), simple 801
  [783–820], lavd 796 [772–815] (1/4), never3 791 [732–813] (1/4),
  bpfland 777 [725–812] (1/4), kairos 775 [746–854] (1/4). All within
  ±3% of simple, inside each config's own spread. Context switches:
  simple 4.9k/s, lavd 5.3k, never3 6.4k, kairos 6.8k, eevdf 7.5k,
  bpfland 7.8k.
- **Light load** (`e7-light.conf`: 500 req/s, no batch, 15 s): p50 kairos
  1.039 ms (5/5), never3 1.053 (5/5), lavd 1.059 (5/5), bpfland 1.125
  (3/5), simple 1.137, eevdf 1.249 (0/5). p99 4.4–4.8 ms for kairos,
  bpfland, lavd and eevdf; never3 7.4 [3.7–35.9], simple 14.4
  [4.5–22.1]. Server CPU per request: bpfland 1,020 µs, eevdf 1,052,
  kairos 1,061, never3 1,085, lavd 1,094, simple 1,149 (500 µs of it is
  the handler's work). System time 3.6–4.4% for the sched_ext
  schedulers, 8.0% for eevdf, whose CPUs 0–2 were also busier (0.67 vs
  0.51–0.55 CPUs).
- **perf bench sched pipe** (`experiments/e7_pipe.sh`, 200k round
  trips): µs per round trip pinned to CPU 0: simple 1.50, never3 1.59,
  kairos 1.65, bpfland 1.81, eevdf 2.00, lavd 2.05. On CPUs 0,1: simple
  1.62, never3 1.64, bpfland 2.00, eevdf 14.6, kairos 15.2, lavd 15.9.
  (The script runs the schedulers in the same order every rep, unlike
  run.sh.)
- **BPF callbacks** (`e7-bpfstats.conf`: 2 zstd, 5 s phases
  500/3500/500/3500 req/s, 2 reps, kernel.bpf_stats_enabled=1, reset to 0
  after): all struct_ops programs together, % of one CPU from load to
  unload: simple 1.46, never3 2.63, kairos 4.11, bpfland 5.53, lavd
  7.28. kairos per call: dispatch 418 ns, select_cpu 363, running 164,
  enqueue 150, runnable 131, stopping 96, quiescent 63; simple: dispatch
  404, select_cpu 247, enqueue 92, running 39, stopping 36. kairos made
  ~37k running() calls/s against simple's ~19k under the same load. The
  1 ms BPF timer isn't counted (bpf_stats counts program entries only).

### 6.21 E3-idle: Linux with SCHED_IDLE batch jobs (2026-11-02)
`results/2026-11-02-e3-idle/`, 6 runs, 18:56–19:13, commit 8086c1f
(clean). eevdfidle = Linux's scheduler with zstd as SCHED_IDLE; kairos as
in E3 (zstd SCHED_BATCH); E3's phases; 3 reps. All runs clean: VM share
0.98–0.99, swap-ins ≤ 40 pages, no reclaim stalls, other processes
≤ 15% of a CPU. Calibration 1,033.
- Medians over 3 reps (eevdfidle / kairos):

| L (ms) | busy p50 | busy >5 ms (%) | quiet >5 ms (%) | MB/s |
|---|---|---|---|---|
| 10 | 1.82 / 0.96 | 14.2 / 4.4 | 1.7 / 2.8 | 413 / 373 |
| 30 | 2.20 / 0.92 | 22.6 / 3.5 | 1.8 / 1.8 | 416 / 392 |
| 100 | 2.56 / 0.98 | 28.1 / 3.2 | 4.2 / 0.8 | 405 / 405 |
| 300 | 2.97 / 0.98 | 33.5 / 8.0 | 1.9 / 2.9 | 390 / 378 |
| 1000 | 2.75 / 0.89 | 30.5 / 2.2 | 2.4 / 0.8 | 386 / 408 |
| 3000 | 2.27 / 1.05 | 22.1 / 8.4 | 1.6 / 2.6 | 378 / 364 |
| 10000 | 2.42 / 0.99 | 23.7 / 11.6 | 1.5 / 1.6 | 398 / 388 |

- Paired: kairos had the lower busy p50 and the smaller busy >5 ms share
  in 3/3 at every L. eevdfidle had more block MB/s in 14 of the 21
  block-rep pairs; over the whole run 414 vs 405 MB/s (median; per rep
  372/370, 414/410, 437/405).
- By time since a burst started, eevdfidle's busy p50 is 1.5 ms in the
  first 2 ms and 2.3–2.9 ms after 5 ms, with no rise-and-fall as under
  SCHED_BATCH (6.19). After a burst ends there's no backlog (quiet
  >5 ms 3.7% in the first 2 ms, against 24% for eevdf with SCHED_BATCH).
- kairos in this batch: busy p50 0.89–1.05 ms as in E3-r. Its 10 s
  block had a slow episode in one rep (p99 283 ms, >5 ms 11.6% vs 4.1%
  in E3-r). Pool 2.98 CPUs, preemptions 7,700–8,600/s.
- Whole run (medians): context switches 44.2k/s (eevdfidle) vs 45.0k;
  idle on CPUs 0–2 0.38 vs 0.37 CPUs; server 1.20 vs 1.26 CPUs.
- Against E3-r's eevdf with SCHED_BATCH (other batch, so rough):
  SCHED_IDLE cut Linux's busy p50 from 2.6–11.3 ms to 1.8–3.0 ms and
  its batch from 412–448 to 378–416 MB/s, and raised its idle time on
  CPUs 0–2 from 0.31 to 0.38 CPUs.

### 6.22 E10: which wakeups preempt (2026-11-04)
`results/2026-11-04-e10-diag/`, 6 runs, 03:30–03:46, commit 98f977e
(clean). kairos as in E3; kpw = kairos with `--preempt-wait-us=250
--preempt-min-run-us=100` (an LC task whose runs average under 100 µs
waits up to 250 µs for a CPU before a borrower is preempted). `--diag`
groups: grpcworker (the request workers), grpcserver (the server's other
threads: Go runtime threads, netpoller, gRPC transport), zstd, other.
E3's phases, 3 reps. Calibration 1,024.
- Checks: VM share 0.98–0.99, other processes 4–13% of a CPU, swap-ins
  ≤ 14 pages. But 6,700–12,000 direct-reclaim stalls and 19–35k
  compaction stalls per run (about 50–80 and 150–250 per second, with
  kswapd not scanning). E3-r and E3-idle had none. kairos's latency and
  batch match E3-r and E3-idle (below), so I use the runs (section 8).
- Preempting runs (runs that began by preempting a borrower), per
  second, median over reps:

| group | quiet (500/s) kairos | kpw | busy (3500/s) kairos | kpw |
|---|---|---|---|---|
| grpcserver | 6,576, mean 7 µs | 3 | 9,470, mean 12 µs | 19 |
| grpcworker | 164, mean 532 µs | 461 | 460, mean 425 µs | 1,192 |
| other | 136, mean 71 µs | 29 | 86, mean 56 µs | 17 |

  98–99% of grpcserver's preempting runs were under 50 µs. So 94–96% of
  kairos's preemptions hand a CPU to a thread that runs ~10 µs and
  sleeps again; the workers that serve requests cause 2–5%.
- Per request, zstd was preempted 4.35 times (quiet) and 0.76 (busy)
  under kairos, 0.73 and 0.19 under kpw. grpcserver threads waited 0.18
  → 0.45 ms per request (quiet) and 0.24 → 0.35 ms (busy) with kpw.
- Whole run, kairos / kpw: preemptions 8,740 / 1,384 per second (587 of
  them by the wait timer); kairos per CPU 3,793 / 2,932 / 2,015 on CPUs
  0–2. Batch 409 / 421 MB/s (per rep 395, 409, 411 / 430, 421, 409).
  Context switches 46.9k / 37.2k per second; idle on CPUs 0–2 0.37 /
  0.34 CPUs. Idle CPUs while a task was queued: 0.004 / 0.0008 CPUs.
- Latency: kairos busy p50 0.88–0.95 ms at every L (E3-r 0.90–0.98),
  kpw 1.07–1.10; kairos lower in 3/3 reps at every L. Block batch: kpw
  higher in 2–3/3 at every L.
- kpw-r2 had a 5 s overload episode from t ≈ 2.5 s: 18–29 runnable
  tasks, the longest wait of any task 1.9 s, batch 23 MB/s, busy p50 up
  to 21 ms, load generator send lag p99 5–8 ms. Reclaim stalls fell to
  1–8 per second during the episode (55–95 before and after it), so as
  far as these counters show, memory was not the trigger. Guess: the
  stalls come with zstd's allocations, which stopped while batch was
  starved. It looks like the slow episodes seen before (sections 6.1,
  6.16).

### 6.23 E11: delayed borrower preemption (2026-11-04)
`results/2026-11-04-e11-delay/`, 35 runs, 03:46–05:24, commit 98f977e
(clean). pwDrN = kairos with `--preempt-wait-us=D
--preempt-min-run-us=N`: an LC task whose runs average under N µs waits
up to D µs for a CPU, then the timer preempts a borrower; the others
preempt at once. pw250 = D 250 µs with every LC task waiting. bpfland;
eevdfidle = Linux with zstd SCHED_IDLE. E3's phases, 5 reps.
Calibration 1,031.
- Checks: VM share 0.98–1.00, other processes 4–7%, swap-ins 0 (one run
  339). Direct-reclaim stalls 88–3,900 per run in every config (medians
  1,055–2,051), fewer than in E10.
- Medians over 5 reps; ranges over the 7 burst lengths; whole-run
  columns are medians over reps (preempt/s includes the timer's):

| config | busy p50 (ms) | busy >5 ms (%) | quiet p50 | MB/s | preempt/s | ctx k/s | idle CPUs 0–2 |
|---|---|---|---|---|---|---|---|
| kairos | 0.94–1.10 | 2.0–7.7 | 0.81–0.90 | 376 | 8,020 | 45.5 | 0.37 |
| pw100r100 | 1.00–1.18 | 2.4–8.6 | 0.85–0.94 | 380 | 2,025 | 38.6 | 0.35 |
| pw250r100 | 1.20–1.25 | 2.4–8.9 | 0.88–0.95 | 392 | 1,318 | 36.5 | 0.34 |
| pw1000r100 | 1.21–1.45 | 3.7–9.5 | 0.88–0.98 | 399 | 841 | 34.1 | 0.33 |
| pw250 | 1.38–1.55 | 6.0–10.9 | 0.90–0.98 | 395 | 839 | 34.4 | 0.33 |
| bpfland | 1.55–1.69 | 14.8–18.1 | 0.93–1.09 | 394 | – | 38.2 | 0.30 |
| eevdfidle | 2.06–3.20 | 18.3–34.3 | 1.14–1.35 | 376 | – | 44.4 | 0.37 |

- Delayed LC wakeups: 6,170–6,880 per second in every pw config. The
  timer had to preempt for 1,330 of them per second with D 100 µs, 578
  with D 250 µs and 11 with D 1 ms; the others got a CPU without a
  preemption first. pw250: all 839 preemptions/s by the timer.
- Paired with kairos (same rep): kairos had the lower busy p50 in 5/5
  reps at every L against pw250r100, pw1000r100 and pw250, and in 3–5/5
  against pw100r100. Whole-run batch per rep: kairos 387, 367, 380, 364,
  376; pw1000r100 407, 395, 399, 377, 402 (higher in 5/5); pw250r100
  5/5; pw250 5/5; pw100r100 2/5 (one tie).
- Paired with bpfland: pw1000r100 had the lower busy p50 in 4–5/5 and
  fewer busy requests over 5 ms in 4–5/5 at every L; block batch higher
  in 17 of 35 block-rep pairs; whole run 399 vs 394 MB/s (bpfland per
  rep 387, 400, 394, 392, 398; pw1000r100 higher in 3/5). pw250r100:
  4–5/5, 4–5/5, 16 of 35. kairos: 5/5, 5/5, 4 of 35.
- Episodes with requests over 50 ms (episodes.py), per run: kairos 0–7
  (median 3), pw1000r100 1–5 (4), pw100r100 3–8 (5), pw250 4–8 (6),
  pw250r100 3–11 (8), eevdfidle 9–14 (11), bpfland 10–18 (15).

### 6.24 E12: ablations, and the pipe test with sync wakeups (2026-11-04)
`results/2026-11-04-e12-ablate/`, 18 runs, 05:24–06:14, commit 98f977e
(clean). Each config is kairos (as in E3) with one option: sync =
`--wake-sync`; percpu = `--lc-queue percpu`; longest, prev =
`--borrower-pick longest`, `prev` (default `first`, the lowest-numbered
borrower CPU). eevdfidle as in E11. E3's phases, 3 reps. Calibration
995 (E10, E11: 1,024–1,031; the fastest of three tries is kept, so the
host was ~3% slower at the start of E12).
- Checks: VM share 0.98–0.99, other processes ≤ 6%, swap-ins ≤ 20.
  Reclaim stalls only in rep 3 (14–253 per run).
- Medians over 3 reps, ranges over the 7 burst lengths:

| config | busy p50 (ms) | busy >5 ms (%) | quiet p50 | MB/s | preempt/s on CPUs 0 / 1 / 2 | ctx k/s |
|---|---|---|---|---|---|---|
| kairos | 0.86–1.04 | 1.8–6.5 | 0.79–0.86 | 389 | 3,253 / 2,494 / 1,697 | 46.3 |
| sync | 0.84–1.04 | 1.3–10.6 | 0.79–0.87 | 380 | 3,352 / 2,630 / 1,805 | 46.4 |
| percpu | 1.46–1.71 | 8.7–13.7 | 0.85–0.94 | 377 | 5,187 / 2,148 / 1,219 | 43.5 |
| longest | 0.87–1.01 | 1.0–3.6 | 0.78–0.88 | 381 | 2,829 / 2,857 / 2,831 | 46.6 |
| prev | 0.85–1.00 | 1.3–5.8 | 0.79–0.86 | 385 | 3,223 / 2,964 / 2,460 | 47.0 |
| eevdfidle | 1.99–2.99 | 17.1–32.0 | 1.11–1.30 | 401 | – | 44.9 |

- percpu: kairos had the lower busy p50 in 3/3 reps at 6 of 7 L (2/3 at
  300 ms) and fewer busy requests over 5 ms in 3/3 at 5 of 7. percpu
  had more system time (11.6% vs 9.4%), more server CPU (1.32 vs 1.25
  CPUs), more runnable tasks on average (11.1 vs 9.1) and less idle
  (0.34 vs 0.38 CPUs). Batch per rep 383, 377, 373 (kairos 334, 389,
  393). percpu's busy p50 is still below eevdfidle's at every L.
- sync: 6 sync wakeups per second took the waker's CPU; no consistent
  change (paired 1–3/3 at every L).
- longest spread preemptions evenly over CPUs 0–2; prev partly. Neither
  changed latency or batch consistently (paired 1–3/3).
- prev had no overload like the one in its smoke run (section 10,
  2026-11-03): 1–6 episodes over 50 ms per run, the worst 237–274 ms
  (kairos 1–7, worst 129–640 ms).
- kairos-r1's batch (334 MB/s) is low: that run had 7 slow episodes,
  the worst 640 ms.
- Pipe (`results/2026-11-04-e12-pipe/`, 06:14–06:16, 5 reps): µs per
  round trip, median [min–max]:

| CPUs | simple | never3 | kairos | kairossync |
|---|---|---|---|---|
| 0 | 1.57 [1.46–1.66] | 1.60 [1.57–1.70] | 1.65 [1.63–1.70] | 1.65 [1.64–1.71] |
| 0,1 | 1.45 [1.41–1.60] | 1.54 [1.52–1.75] | 14.86 [12.94–15.19] | 1.58 [1.57–1.63] |

### 6.25 G: Redis, and G-nb without batch (2026-11-04)
`results/2026-11-04-g-redis/`, 30 runs, 06:16–06:53, commit 98f977e
(clean). Three single-threaded redis-server instances on CPUs 0–2
(10,000 keys of 1 KB); the load generator on CPU 3 sends pipelined GETs
of random keys (4 connections per instance); 5 s at 2,000 and 20,000
GETs/s, 6 times; 3 zstd jobs (SCHED_BATCH; eevdfidle SCHED_IDLE).
kairos as in E3; pw50, pw200 = `--preempt-wait-us` 50, 200 (Redis's
runs are short, so every wakeup waits). 5 reps. G-nb:
`results/2026-11-04-g-redis-nobatch/`, 9 runs, 07:02–07:14, commit
47801f7: eevdf, bpfland, kairos, no batch, 3 reps.
- Checks: VM share 0.96 (eevdf, eevdfidle, bpfland) and 0.98–0.99
  (kairos, pw*); in G-nb 0.96–1.00. Other processes ≤ 12%, no swapping,
  no reclaim stalls.
- The load generator falls behind: its send lag p99 was 1–8 ms in most
  phases, with single phases at 30–440 ms, in every config (G-nb: mostly
  0.1–1.6 ms, single phases 7–43 ms). Latency counts from the intended
  send time, so p99 and p99.9 measure the load generator as much as
  Redis. I compare medians only.
- Medians over reps; latency ranges over the 6 phases of each rate
  (compare.py prints p50 to 0.1 ms); whole-run batch and CPU:

| config | p50 2,000/s (ms) | p50 20,000/s | p99 20,000/s | MB/s | preempt/s | Redis CPU | system time |
|---|---|---|---|---|---|---|---|
| eevdf | 0.5–0.6 | 0.5 | 3–4 | 755 | – | 0.11 | 9.8% |
| eevdfidle | 0.5 | 0.5 | 2–4 | 740 | – | 0.11 | 10.1% |
| bpfland | 0.6 | 0.5 | 5–27 | 746 | – | 0.12 | 9.1% |
| kairos | 0.1 | 0.1–0.2 | 3–7 | 707 | 11,190 | 0.27 | 19.3% |
| pw50 | 0.2 | 0.3–0.4 | 7–37 | 717 | 4,217 | 0.17 | 18.7% |
| pw200 | 0.3–0.4 | 0.3 | 3–6 | 732 | 2,420 | 0.14 | 17.0% |

- Batch per rep: eevdf 820, 746, 735, 756, 755; kairos 649, 653, 707,
  707, 711. p99: eevdf below kairos in 1–4/5 reps per phase (no
  consistent order).
- kairos's preemptions fell on CPU 0 for 92% (10,293 of 11,190 per
  second) with the default borrower choice.
- G-nb: p50 0.2 ms at 2,000/s and 0.1 ms at 20,000/s for all three.
  Redis CPU 0.22 (eevdf), 0.24 (bpfland), 0.37 (kairos).

### 6.26 Azure replay: structure at the predictor's timescale? (offline, 2026-11-04)
Autocorrelation of the E2a replay rates
(`workloads/traces/azure2021-pick3-seed1.phases`, one rate per 100 ms
phase = 10 s of trace), and the mean absolute error (req/s) of three
predictors of the next 200 ms's maximum rate, the predictor's horizon.
Script: `~/kairos-data/scratchpad-2026-11-04/azure_acf.py` (not in the
repo).

| hour | mean req/s | acf lag 1 | acf lag 6 (1 min of trace) | MAE: persistence / mean of last 5 / one minute earlier |
|---|---|---|---|---|
| 36 | 1,014 | +0.00 | +0.14 | 316 / 228 / 241 |
| 54 | 1,148 | +0.58 | +0.13 | 463 / 485 / 608 |
| 284 | 372 | +0.32 | +0.04 | 85 / 89 / 121 |

- Apart from hour 54's lags 1–3 (its sustained busy stretch) and hour
  284's lag 1, every autocorrelation at lags 1–12 is between −0.12 and
  +0.25.
- "The same phase one minute of trace earlier" is worse than persistence
  in 3/3 hours.
- The raw trace has a per-minute pattern (5.7% of arrivals in second :00,
  section 8), but the 10 s bins average it away.

### 6.27 Azure trace at 1 s: does a learned predictor beat simple ones? (offline, 2026-11-04)
`ml/azure_predict.py` (1018388); outputs and commands in
`results/2026-11-04-azure-predict/`.

Method:
- Series: what `azure_phases.py --bin-s 1` would replay at 10x, one
  100 ms phase per second of trace, each hour scaled so its busiest
  second is at 3500 req/s, floor 300.
- LC demand: 1 ms of CPU per request. This comes from a fit on E2a
  kairos-r2's sched_stats.csv: lc_demand = 0.19 + 0.00104 × rate CPUs,
  R² 0.26.
- One sample per phase. History K = 5 phases (500 ms), horizon H = 2
  (200 ms), period P = 60 (6 s, one minute of trace).
- The tree is train_burst.py's CART (depth 5, min leaf 50). Its inputs
  are the last 5 phases, the scheduler's EWMA, and the demand in the
  target window 1–5 periods earlier. It is trained on 24 random hours
  (seed 2) with at least the median arrivals, and tested on other hours.
- Pool = ceil(demand + 0.5) in [1, 8]. "Under" = share of samples whose
  pool is smaller than the next 200 ms needs.
- Each simple predictor is compared at the tree's cost: with the extra
  buffer at which it gives LC as many extra CPUs as max(ewma, tree),
  interpolated between 0.01 CPU steps.

Results:
- At 1 s the trace is sparse. The test hours have 1.4–2.2 arrivals per
  second outside second :00 and 4.2–7.6 at :00. 60–78% of phases are at
  the floor, and mean demand is 0.35–0.38 CPUs. Autocorrelation at lag 60
  (one minute) is +0.03 to +0.14.

| test hour | tree: under (at :00) | ewma + buffer | seasonal mean (M = 5) + buffer | best simple predictor at the tree's cost |
|---|---|---|---|---|
| 36 | 8.9% (33%) | 10.0% (48%) | 8.9% (17%) | 8.7%, max(ewma, seasonal mean) |
| 54 | 11.2% (37%) | 10.7% (44%) | 17.9% (17%) | 8.2%, mean of the last 5 |
| 284 | 7.4% (26%) | 7.1% (31%) | 9.4% (13%) | 7.1%, mean of the last 5 and ewma |

- 12 other test hours (24, 37, 56, 70, 97, 103, 218, 233, 237, 279, 281,
  321; training hours redrawn without them):
  - Against the best simple predictor, the tree was lower in 2/12 (237:
    10.5 vs 10.7%; 321: 4.1 vs 4.4%).
  - Against ewma + buffer, it was lower in 6/12, equal in 2 and higher
    in 4.
  - The mean of the last 5 plus buffer was the best simple predictor in
    8/12, max(ewma, seasonal mean) in 3/12.
- Sensitivity, on E2a's hours: train seeds 3 and 4; depths 4 and 6; min
  leaf 20; M = 10; peak 7000 req/s.
  - The tree was never lower than the best simple predictor (0 of 21
    hour-variant pairs, one tie at 7.1%).
  - `--buffer 1.5` gives the same numbers as 0.5, because a whole CPU of
    extra buffer moves both pools up by one while they stay under 8 CPUs.
  - With 3 CPUs instead of 8 the results are the same within 0.1 points.
- At the :00 onsets (108 of 3,298 samples per hour, 3.3%), the seasonal
  mean is under-provisioned least.
  - E2a's hours: 13–17%, against 26–37% for the tree and 31–48% for
    ewma + buffer at the same cost.
  - The 12 other hours: 8–29%, against 18–46% (tree) and 21–62% (ewma).
  - At the other phases the seasonal mean is worse than the reactive
    predictors.
- MAE (CPUs) on E2a's hours: the mean of the last 5 is lowest in 3/3
  (0.065–0.130); the tree's is 0.089–0.152.

### 6.28 Duplicate borrower picks (DS, smoke run with the host slowing the VM; 2026-11-04)
`results/2026-11-04-dup-smoke/`, 1 rep, commit 3067b18, E3's phases,
kernel 7.0.0-42. Calibration 1,019 iterations/µs.

Not a measurement of latency or batch: the host slowed the VM during all
three runs.
- Server CPU per request: 910–1,114 µs, against 677 in E11 kairos-r1 and
  674 in FS kairos on the same kernel.
- Load generator CPU per request: 190–222 µs, against 137–148.
- VM share: 0.94–0.95.
- Results: kairos busy p50 5.1 ms (E11: 1.00–1.05); pw1000r100 109 ms;
  batch 88–160 MB/s.

Counters, per second over the load:

| config | preemptions | dup_pick | dup_alt |
|---|---|---|---|
| kairos | 4,081 | 728 (17.8%) | 364 (8.9%) |
| fixed3 | 3,587 | 591 (16.5%) | 313 (8.7%) |
| pw1000r100 | 241, + 18 by the timer | 18 (7.0%) | 7.8 (3.0%) |

- **The preemption rate** is half E11's (8,020/s) with the same phases,
  and so are the LC and batch queue inserts (21k vs 30k per second).
  Not explained; the slowed host is the likeliest cause (guess).
- **These shares are probably higher than on a quiet host** (guess). A
  CPU stays marked from the decision until it switches, which includes
  the IPI to it, and that takes longer when the host delays vCPUs. A
  clean rerun decides.
- **What a duplicate costs.** The second LC task is inserted at the head
  of the same CPU's local queue (`SCX_ENQ_PREEMPT`). The first task then
  waits for the second's run, while another borrower keeps running batch
  work. Most preempting runs last 7–12 µs (E10), so a duplicate likely
  costs little unless a worker (~0.5 ms runs) is involved (guess).
- **fixed3 matched kairos:** its LC pool was at 3.00 CPUs, as was
  kairos's.

### 6.29 Duplicate borrower picks on a clean run (DS2; 2026-11-04)
`results/2026-11-04-dup-smoke2/`, 1 rep, commit 60c15d8, E3's phases,
kernel 7.0.0-42. Mac idle, on `caffeinate`.

The runs are clean:
- VM share 0.98;
- server CPU per request 643–671 µs (DS: 910–1,114; FS: 674);
- other processes 9–13% of a CPU;
- swap-ins 0–901, direct-reclaim stalls 34–603, all under F's flags.

Counters, per second over the load (`runlib.sched_stats`):

| config | preemptions | dup_pick | dup_alt | no_victim |
|---|---|---|---|---|
| kairos | 8,061 | 1,489 (18.5%) | 711 (8.8%) | 19,827 |
| fixed3 | 8,415 | 1,556 (18.5%) | 735 (8.7%) | 20,105 |
| pw1000r100 | 901, + 28 by the timer | 30 (3.2%) | 7 (0.8%) | 17,121 |

- **The preemption rate is back to E11's** (8,020/s), so DS's halved rate
  came from the slow host.
- **The duplicate shares are not.** kairos's dup_alt is 8.8% here, against
  8.9% in DS. The guess in 6.28 that the slow host inflated them was
  wrong: about 1 in 5 of kairos's borrower preemptions picks a CPU that
  is already being preempted, and in about half of those another
  borrower was free.
- **By the rule in 5.1** (dup_alt ≥ 1%), the fix is tested first: E14,
  `--claim-borrower`.
- **pw1000r100 is under 1%.** Most of its LC wakeups don't preempt at
  once.
- **Latency and batch** (analysed after E14):
  - kairos: busy p50 0.99 ms, busy >5 ms 12.7%, batch 381 MB/s;
  - fixed3: 0.96 ms, 11.3%, 392 MB/s;
  - pw1000r100: 1.29 ms, 12.6%, 410 MB/s.

  The >5 ms shares are 3x E14's (kairos 3.9%, pw1000r100 4.9%) and
  above E11's per-L ranges. This is one rep each, the first runs after
  the VM had been idle for hours. Not explained.

### 6.30 One borrower preemption per CPU at a time (E14; 2026-11-04)
`results/2026-11-04-claim-burst/`, `results/2026-11-04-claim-azure/`, 10
reps, commit e11c590, kernel 7.0.0-42, Mac idle on `caffeinate`. kclaim
and pwclaim are kairos and pw1000r100 with `--claim-borrower`.

**Run checks** (rules in 5.1):
- All 80 runs: VM share 0.98, other processes ≤ 14% of a CPU, direct-reclaim
  stalls ≤ 21.
- Two runs had more than 1,000 swap-ins: claim-burst kairos-r1 (1,593)
  and claim-azure pw1000r100-r7 (1,874).
- Both are references, so by the per-run rule the kairos pair on the
  burst sweep and the pw1000r100 pair on the Azure replay have n = 9.
- With all 10 reps the conclusions are the same: kclaim busy p50 −0.03
  ms, p = 0.004; pwclaim on Azure: every primary p ≥ 0.69.

**Burst sweep**, primary metrics (`stats.py --ref X --configs X Xclaim`;
median of the paired differences, bootstrap 95% CI, in how many reps
the fix was better, exact Wilcoxon p):

| pair | busy p50 (ms) | busy >5 ms (points) | batch, whole load (MB/s) |
|---|---|---|---|
| kclaim − kairos (n = 9) | −0.02 [−0.05, −0.02], 8/9, **p = 0.008** | +0.2 [−1.4, +1.7], 3/9, p = 0.57 | −1 [−10, +2], 3/9, p = 0.43 |
| pwclaim − pw1000r100 (n = 10) | +0.01 [−0.01, +0.02], 2/10, p = 0.38 | +0.9 [−0.3, +2.2], 3/10, p = 0.065 | −4 [−8, +0], 3/10, p = 0.065 |

Reference medians: kairos busy p50 0.93 ms, >5 ms 3.9%, 404 MB/s;
pw1000r100 1.20 ms, 4.9%, 435 MB/s.

**Azure replay**, primary metrics:

| pair | p50, all (ms) | >5 ms, all (points) | batch (MB/s) |
|---|---|---|---|
| kclaim − kairos (n = 10) | +0.00, 5/10, p = 0.70 | +0.3, 4/10, p = 0.38 | −5, 3/10, p = 0.11 |
| pwclaim − pw1000r100 (n = 9) | +0.00, 4/9, p = 0.73 | −0.1, 5/9, p = 1.0 | −1, 4/9, p = 0.82 |

**Per burst length** (descriptive; 21 tests per pair, unadjusted):
- kclaim's busy p50 is lower at every L (by 2–5%; 5–8 of 9 reps better);
  p < 0.05 at L 10, 30 and 10,000 ms.
- pwclaim is worse at the two shortest bursts. At L 10 and 30 ms:
  - busy >5 ms +1.2 and +1.6 points (p = 0.027, 0.020);
  - block batch −17 and −18 MB/s, −4% (p = 0.037, 0.049);
  - busy p50 +3% at L 30 ms (p = 0.049).

  At L ≥ 100 ms nothing differs.

**Counters**, medians over reps, per second over the load:

| config | preemptions | dup_pick | dup_alt | no_victim |
|---|---|---|---|---|
| burst: kairos | 8,629 | 18.8% | 8.8% | 20,636 |
| burst: kclaim | 8,046 | 22.0% | 9.5% | 22,048 |
| burst: pw1000r100 | 935 (14 by the timer) | 2.7% | 0.4% | 17,037 |
| burst: pwclaim | 900 (11) | 4.6% | 1.0% | 17,060 |
| Azure: kairos | 9,188 | 15.6% | 9.3% | 10,575 |
| Azure: kclaim | 8,961 | 18.8% | 10.3% | 11,580 |
| Azure: pw1000r100 | 778 (19) | 1.1% | 0.5% | 8,769 |
| Azure: pwclaim | 766 (18) | 3.4% | 1.6% | 8,764 |

- With the option, dup_alt counts redirections to another borrower and
  dup_pick − dup_alt counts wakeups sent to LC_DSQ instead. kclaim on the
  burst sweep: about 760 redirected and 1,000 queued per second, so
  no_victim is +1,400/s and preemptions are −580/s.
- pwclaim changes about 40 of its ~900 decisions per second.

**Decision by the rule in 5.1:** neither variant with the fix is worse
with p < 0.05 on any primary metric of either workload, so
`--claim-borrower` is adopted for kairos and for pw1000r100. F's configs
(`final-burst.conf`, `final-azure.conf`) now pass it to kairos,
pw1000r100 and fixed3, under the same names.
- **Caveat for pw1000r100:** on the burst sweep two primary metrics lean
  worse (p = 0.065), and the shortest bursts are worse at unadjusted
  p < 0.05. Aayush can still keep pw1000r100 without the fix before F
  runs; the rule doesn't require it.

**Across kernels** (a guess): E14's kairos batch, 404 MB/s, is 7% above
E11's 376, and pw1000r100's, 435, is 9% above 399. Same phases. E11 ran
on 7.0.0-38, E14 on -42. Not tested.

### 6.31 The server at GOMAXPROCS 3 (GMP3; 2026-11-04)
`results/2026-11-04-gmp3-burst/`, 6 reps, commit 6fefc35, E3's phases.
The server runs GOMAXPROCS 3, Go's default under its 3-CPU mask, instead
of the 16 used since E1b. kairos and pw1000r100 run with
`--claim-borrower`.

**Run checks:** none flagged. VM share 0.98–1.00, other processes ≤ 18%
of a CPU, swap-ins ≤ 173, no direct-reclaim stalls. Server CPU per
request in rep 1: 596–640 µs (claim-burst kairos-r2: 648).

**Results**, medians over 6 reps; the diff is paired, against kairos:

| config | busy p50 (ms) | busy >5 ms (%) | quiet p50 (ms) | batch (MB/s) |
|---|---|---|---|---|
| kairos | 0.82 | 3.3 | 0.83 | 411 |
| pw1000r100 | 1.13 (+37%) | 4.7 | 0.93 | 429 (+5%) |
| bpfland | 1.56 (+89%) | 8.5 | 0.98 | 443 (+8%) |
| eevdf | 227 | 92 | 7.9 | 482 (+17%) |

- **All four orderings hold in 6/6 reps.** kairos had the lowest busy
  p50, busy >5 ms and quiet p50 against every other config, and the
  least batch. pw1000r100 beat bpfland on busy p50 and >5 ms in 6/6. Its
  batch was below bpfland's in 5/6 (p = 0.22).
- **Not significant after Holm.** At n = 6 the smallest exact p is
  2/64 = 0.031, and Holm over 3 comparisons makes that 0.094. That rules
  out any rejection, which I missed when choosing 6 reps: it takes n ≥ 7
  (2/128 × 3 = 0.047). The results are descriptive.
- **The same ordering as at GOMAXPROCS 16** (E3: kairos 0.90–0.98 ms,
  bpfland 1.33–1.64 per L). kairos's busy p50 is lower here than in
  E14's GOMAXPROCS 16 runs (0.82 vs 0.91 ms with the fix). That
  comparison crosses experiments and isn't paired.
- **EEVDF is overloaded at GOMAXPROCS 3.** Its busy p50 grows with the
  burst length: 6.2 ms at L 10 ms, 103 ms at 300 ms, 2,684 ms at 10 s.
  So the server falls behind for as long as the burst lasts. At
  GOMAXPROCS 16 (E3) it was 2.6–11.3 ms.
- Counters: kairos 8,215 preemptions/s (dup_pick 19.2%, dup_alt 8.8%,
  redirected by the fix); pw1000r100 891/s. LC pool at 3.00 CPUs in
  both.

### 6.32 Final evaluation (F; 2026-11-05)
`results/2026-11-05-final-burst/` (9 configs) and
`results/2026-11-05-final-azure/` (8 configs), 10 reps each, commit
b10fe6d, kernel 7.0.0-42. It ran 02:30–09:25 with the Mac on
`caffeinate` and both apt timers stopped. kairos, pw1000r100 and fixed3
run with `--claim-borrower`. Tests: `stats.py --ref kairos pw1000r100`
(rules in 5.1). Holm is applied within each metric and reference, over
8 comparisons on the burst sweep and 7 on the Azure replay.

**Run checks** (rules in 5.1): no run flagged, so none is left out.
VM share ≥ 0.97, other processes ≤ 14% (burst) and ≤ 17% (Azure) of a
CPU, swap-ins ≤ 81, direct-reclaim stalls ≤ 54.

The checks don't flag two things (section 8):
- **My wait process** (`tail --pid`, started to notice the end of the
  run) ran in every run. Config medians: 3.5–4.5% of a CPU under EEVDF,
  5.6–8.3% under the sched_ext schedulers.
- **The server used more CPU per request than in E14**, the day before,
  with the same configs:
  - config medians 666–729 µs on the burst sweep (E14: 637–664) and
    763–835 on the Azure replay (E14: 705–763);
  - rep medians 670–733 µs all night on the burst sweep, with no trend.

  Batch was lower too:
  - kairos 324 MB/s against kclaim's 402 in E14 (−19%);
  - pw1000r100 355 against pwclaim's 431 (−18%);
  - on the Azure replay −12% and −11%.

**Burst sweep**, primary metrics, medians over 10 reps:

| config | busy p50 (ms) | busy >5 ms (%) | batch, whole load (MB/s) |
|---|---|---|---|
| kairos | 1.10 | 7.6 | 324 |
| fixed3 | 1.08 | 7.9 | 326 |
| pw1000r100 | 1.41 | 9.2 | 355 |
| bpfland | 1.58 | 16.2 | 360 |
| eevdfidle | 2.37 | 24.0 | 341 |
| never3 | 3.58 | 34.8 | 374 |
| eevdf | 4.11 | 43.3 | 365 |
| lavdperf | 8.52 | 72.2 | 388 |
| lavd | 8.62 | 73.4 | 386 |

**Against kairos:**
- Busy p50: every baseline and pw1000r100 is higher in 10/10 reps (Holm
  p = 0.016 each, the smallest possible with 8 comparisons).
- Busy >5 ms: every baseline is higher (Holm 0.016). pw1000r100 is +2.5
  points and worse in 7/10 (p = 0.027, Holm 0.055).
- Batch is higher, all with Holm ≤ 0.03, for:
  - bpfland +31 MB/s (+10%);
  - eevdf +37, lavd +54, lavdperf +53, never3 +50;
  - pw1000r100 +31 (10/10).

  eevdfidle: +11 (Holm 0.17).

**fixed3 matches kairos.** No primary metric differs (p ≥ 0.43).
- kairos's LC pool held 3 CPUs in every 1 s sample of every run
  (`nr_lc`), so the two configs ran the same pool.
- In kairos-r1 the LC demand estimate did fall in quiet phases: 1 s
  samples p10 0.70, median 3.4 CPUs. The pool didn't shrink anyway (why:
  section 7).
- On the Azure replay too, kairos's and pw1000r100's pools held 3 CPUs
  in every 1 s sample of every run.

**Against pw1000r100:**
- bpfland:
  - busy p50 +0.17 ms (+12%), higher in 10/10 (Holm 0.016);
  - busy >5 ms +7.7 points (Holm 0.016);
  - batch −0 MB/s [−9, +13], 5/10, p = 0.92.
- kairos and fixed3: 9% less batch (−31 and −30 MB/s, 0/10, Holm 0.016).
- lavd (+25) and never3 (+13): more batch (Holm ≤ 0.03). lavdperf +30
  (Holm 0.055).

**Main configuration** (rule in 5.1): on the burst sweep against
bpfland, pw1000r100's busy p50 is lower (Holm p = 0.016) and its batch
is not lower (p = 0.92). **pw1000r100 is the main configuration.**

**Per burst length** (byburst.py; descriptive):
- kairos or fixed3 has the lowest busy p50 at every L (kairos 1.00–1.20
  ms).
- pw1000r100 is below bpfland at every L: 1.34–1.49 ms against
  1.43–1.92.
- kairos's worst L is 30 ms: busy >5 ms 8.1% (2.8–11.6 over reps),
  median p99 194 ms. pw1000r100 has 6.4% there. Not explained;
  episodes.py was not run.

**Azure replay**, primary metrics (p99 descriptive), medians over 10
reps:

| config | p50 (ms) | >5 ms (%) | batch (MB/s) | p99 (ms) |
|---|---|---|---|---|
| kairos | 0.87 | 1.9 | 456 | 8.3 |
| pw1000r100 | 1.02 | 2.6 | 487 | 11.1 |
| bpfland | 1.14 | 3.7 | 476 | 15.1 |
| eevdfidle | 1.44 | 5.9 | 433 | 21.4 |
| never3 | 1.52 | 10.6 | 508 | 25.4 |
| lavdperf | 1.55 | 16.9 | 493 | 90.2 |
| lavd | 1.58 | 18.3 | 486 | 84.6 |
| eevdf | 1.66 | 13.5 | 452 | 50.5 |

**Against kairos:**
- Every config has a higher p50 and >5 ms share (Holm ≤ 0.037).
  pw1000r100's >5 ms is +0.5 points, better in 1/10.
- Batch is higher, all with Holm 0.014, for bpfland (+26), lavd (+40),
  lavdperf (+51), never3 (+53) and pw1000r100 (+33).
- eevdf (+12) and eevdfidle (−11) don't differ.

**Against pw1000r100:**
- bpfland:
  - p50 +0.12 ms (+12%), better in 0/10 (Holm 0.014);
  - >5 ms +1.2 points, better in 2/10 (p = 0.16);
  - batch −3 MB/s (p = 0.70).
- kairos: p50 −0.14 ms (Holm 0.014), >5 ms −0.5 points (Holm 0.074),
  batch −33 MB/s (Holm 0.014).

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

E2 pilots (section 6.8):
- The switch works mechanically after two fixed-point/smoothing fixes:
  BATCH when quiet, LATENCY when busy, with hysteresis.
- With 2 zstd jobs (4 compression threads on 3 CPUs) BATCH mode always
  misses a 1 ms target, so adaptive correctly stays in LATENCY: it can
  only gain where batch work leaves a CPU free at low request load.
- With 1 zstd job batchvic preempts little when quiet, so BATCH has
  little batch throughput to win back. In these workloads class-aware
  preemption seems to capture most of the benefit and the mode switch
  adds little (guess, from single runs; E2 with reps must check). The
  room for the switch grows with the cost of preempting batch work and
  with a looser target.
- The hold doubling makes the return to BATCH slow after a busy phase;
  `--hold-max-ms` and the reset rule are knobs to revisit.

E2 (section 6.9):
- With 1 zstd job the switch does what it was built to do (BATCH when
  quiet, LATENCY 98–100% of busy phases) and matches batchvic's latency
  in busy phases. It gains little: when quiet, batchvic already keeps
  97–98% of never's batch throughput, so BATCH mode wins back at most
  ~2.5%. The doubled hold keeps it in LATENCY for 0.3–0.6 s of each second
  in later quiet phases. Its quiet p99 is slightly higher (4–7 vs 3 ms),
  likely from the 20 ms slices in BATCH mode (guess).
- With 2 zstd jobs, the late share of wakeups doesn't track request
  latency: with a 10 ms target almost no wakeup was late while requests
  took 127–184 ms at p99. Possible reasons, none measured: (1) a server
  thread that is preempted or runs out of slice is queued again without
  SCX_ENQ_WAKEUP, so its wait isn't counted; (2) one request crosses
  several threads (network poller, worker, reply), and waits below the
  target add up; (3) goroutines also queue inside the Go runtime, where
  the kernel can't see them. zstd's main and I/O threads also add
  wakeups that are rarely late, which dilutes the share.
- So the target acts as a knob between never (10 ms) and batchvic (1 ms)
  rather than a latency bound. At 5 ms it ends in between, and its busy
  p99 is worse than both batchvic and the static never3: the BATCH stints
  in busy phases (20 ms slices) add tail spikes. It does win 3–9% batch
  MB/s over batchvic in quiet phases, at quiet p99 13–18 vs 5–7 ms.
- Static never3 (no wakeup preemption, 3 ms slices) is a strong baseline:
  with 2 zstd its busy p99 is 3–19 ms above batchvic's, with 18–36%
  more batch throughput.
- In E2 no adaptive configuration beats the best static one on both
  latency and throughput, apart from small quiet-phase batch gains.
  Batch-only victims remain the mechanism that moves request latency.

E2 diagnostics (section 6.10):
- The wakeup-only signal doesn't miss a kind of wait: the server's
  threads wait almost only after wakeups. The guess that preempted or
  slice-expired server threads hide their waits (section 7, E2) was wrong.
- The signal measures the right events but at the wrong scale. Under
  BATCH mode with 2 zstd jobs a woken worker waits 2–5 ms (until some
  zstd thread's 20 ms slice ends or it sleeps), rarely 10 ms. That
  explains the three targets: with 1 ms, 81% of worker wakeups are late
  (15–16% of all wakeups in E2's never runs), so adaptive stays in
  LATENCY; with 5 ms, 7% of worker wakeups and fewer of the others, so
  the share sits near the 5% threshold and a5 flaps in and out of BATCH;
  with 10 ms, 1.2% of worker wakeups, so a10 never leaves BATCH.
- Requests are far slower than any single wait. Per request the server's
  threads wait 1.3–2.8 ms in total, but the median request takes 2–10 ms
  and the tail hundreds. Measured contributors: queueing for a worker
  (2.3 ms at p50 under never/a10, close to the workers' wakeup waits),
  the way in (1.3–1.7 ms at p50) and, in the tail, the way in and back
  (81–97% of the slowest 1%, every config). Goroutine waits of ≥ 20 ms
  inside Go are 25x more common under never than under batchvic.
  Likely reasons (not measured one by one): a request crosses several
  threads and goroutines, so waits add up; one late wakeup of a network
  or runtime thread delays every request queued behind it, which a
  per-wakeup share counts once; and the load generator is scheduled by
  the same scheduler (its send lag p99 under never was 27 ms in E2, vs 7
  under batchvic), so part of "in" and "out" is the instrument itself.
- So a share of late wakeups with a fixed target can't track request
  latency: the factor between the two depends on load and on the mode.
  A signal closer to what requests feel would weight waits by how many
  requests are behind them, e.g. total waiting time of the service's
  threads per second (= average number of them waiting), or take
  latency from the service itself, as colocation controllers such as
  Heracles do (section 2).
- The slice length sets the wait: workers' wakeup waits ≥ 2 ms were 66%
  of waits with 20 ms slices and 22% with 3 ms (both without
  preemption). A flexible slice (short while latency threads wait, long
  otherwise) would bound waits without preempting zstd; bpfland, lavd
  and CFS do this (section 2.9).
- Where the throughput goes: with preemption zstd is preempted ~3,600
  times/s in busy phases (batchvic), and the server's non-worker threads
  use 2.6x more CPU (38.5 vs 14–15% of a CPU), probably Go's threads
  spinning more when wakeups are fast (guess). Mode switching itself is
  a variable write; what cost a5 its latency was the BATCH stints during
  busy phases (seconds with BATCH time: p50 4.5 ms vs 1.6 ms in seconds
  fully in LATENCY, pooled over E2's reps).

Soft CPU split pilots (section 6.11), tentative:
- With GOMAXPROCS 16, the split cut both kernel waits and long Go waits
  the most and kept batch throughput near never3's, but its runs were
  also the quietest; a clean rerun is needed before believing it.
- Not preempting borrowers (splitnp, 1 ms borrow slices) lost most of the
  latency gain: with 3 CPUs the LC pool is all 3 CPUs in busy phases, so
  zstd only ever borrows and LC wakeups mostly find borrowers.
- The split didn't reduce zstd preemptions (~5,500/s busy, like
  batchvicp): with 3 CPUs there is no spare CPU to keep idle; the
  split's benefit, if real, comes from LC tasks having priority in their
  own queue and pool, not from fewer preemptions. With more CPUs a pool
  could keep idle CPUs without lending them.

E2s (section 6.12):
- On 3 CPUs the split is about as good as preempting batch tasks
  everywhere (batchvicp): the same latency, ~4% more batch throughput when
  quiet, and 63% (quiet) / 24% (busy) fewer preemptions. When quiet the LC
  pool is 1 CPU, so zstd runs undisturbed on the other two and only the
  borrower on the LC CPU is preempted.
- It doesn't escape the trade-off with never3 when busy: the service needs
  ~2.2 CPUs, so the pool is all 3 CPUs and zstd only borrows (preempted
  ~10,000 times a second); never3 keeps 30% more batch throughput at 4–5x
  the median latency. The mechanism that makes CPU splitting work in
  Heracles or PerfIso, spare idle CPUs while the batch CPUs stay
  untouched, needs more CPUs than the service uses; here there is no
  spare CPU in busy phases. More CPUs would test that (guess).
- The SCHED_BATCH "collapse" in the pilots was noise; with a quiet Mac
  Linux behaves the same with zstd as SCHED_BATCH or SCHED_OTHER.

ML for the split (section 6.13):
- The tree learned to ask for ~2 CPUs whenever there was recent LC
  activity: on patterns it didn't see it is no better than adding ~1 CPU
  of buffer to the reactive estimate, and online it behaves like a
  1.5 CPU buffer (pool at 3 CPUs ~95% of the time). Random burst onsets
  can't be predicted from the last 500 ms (expected, section 2.6); the
  model's features can't see E2's 5 s period either.
- On 3 CPUs a full LC pool costs no batch throughput, because zstd
  borrows every CPU the service leaves idle, while a small pool hurts the
  service's quiet-phase tails (LC tasks confined to one LC CPU wait behind
  each other and borrowers). So pool sizing, reactive or predicted, has
  nothing to gain here: the best split setting is the largest pool,
  which is service priority + lending + borrower preemption. That is
  about batchvicp with a few % more batch throughput when quiet.
- This matches E2 and E2s: on 3 CPUs the adaptive parts (mode switching,
  pool sizing, prediction) have little room, because there is no spare
  capacity to manage and bursts aren't predictable at sub-second scale.
  Machines with more CPUs than the service needs, or loads with
  predictable structure (periodic arrivals), are where they could matter
  (not tested).

E2-final (section 6.14):
- KairOS has the lowest median and the fewest requests over 2–10 ms when
  busy, against every baseline. The closest baseline, bpfland, doesn't
  preempt at all: its slices are 1 ms (divided by the number of waiting
  tasks) and it orders tasks by a deadline that favours ones that slept,
  so a woken service thread waits at most about one short slice (from
  scx_bpfland's source, `main.bpf.c`; not measured here). That gives a
  1.4 ms median with no preemption. Linux (2.1 ms base slice) and
  never3 (3 ms) wait for longer slices; lavd's ~7 ms median in both modes
  is not explained (not investigated).
- KairOS pays for it in batch throughput: 7–9% when quiet and 13–28% when
  busy vs bpfland. Most of that is CPU time zstd didn't get, not slower
  zstd. Against bpfland, the server uses 4–6% of a CPU more (as in E2:
  probably Go's threads spinning more when wakeups are fast; guess).
  The server's CPUs are idle 5–6% of a CPU more, and kernel time is
  higher with 18–26% more context switches. Rough estimate: zstd CPU ≈ 3
  CPUs − server − idle − other gives kairos ~8–9% less zstd CPU time than
  bpfland.
- The extra idle time comes with preemption: always, batchvicp and
  kairos all show it, never3 doesn't. A likely cause is a gap in work
  conservation. A preempted task goes back into a shared queue, and no
  idle CPU is woken for it, while the CPU of the thread that did the
  waking may have gone idle in the meantime (a guess, but consistent with
  the code: scx_kairos never kicked a CPU, bpfland does). Fix in
  7f1a122 (section 4), tested in E2k.
- Busy p99 still doesn't rank configs (episodes in single reps); the
  share of requests over a threshold and p95 do, and agree with p50.
- The three degraded runs show a host-side slowdown (guess): CPU time per
  request rose 1.5–1.8x for both the server and the load generator,
  which run on different CPUs under different policies, in consecutive
  runs, regardless of config. The in-VM "other processes" check can't
  see this.

E2k rerun (section 6.16):
- The latency ranking from E2-final holds on a clean run with full
  per-request work: KairOS's busy median is 0.6x bpfland's, and it has
  3–5x fewer busy requests over 5 ms and about half the busy p95.
- The missing-kick explanation for the extra idle time (above) is not
  supported. Kicks fire ~3,800 times a second, but total idle on the
  server's CPUs and batch throughput don't change. They move idle time
  from CPU 2 to CPUs 0 and 1 instead. So kicks are off by default again.
- The extra idle time comes with borrower preemption. kairosnp (kicks,
  no borrower preemption) has bpfland's idle time, fewer context switches
  than bpfland, and slightly more batch throughput. In the preempting
  configs the idle time is uneven, with CPU 0 idle most. Guess, from the
  code: `split_pick_borrower()` scans from CPU 0 and preempts the first LC
  CPU running a batch task, so CPU 0 is preempted most often. It is not
  known why this leaves CPUs idle while batch loses CPU time. Measuring
  time spent idle while a queue holds a task would show whether it is a
  work-conservation gap. Preemptions per CPU are not measured either.
- Borrower preemption is what makes KairOS fast. Without it, with 1 ms
  borrow slices, a woken LC task waits for a borrower's slice to end:
  busy median 2.2–2.4 ms, worse than bpfland (1.4–1.5). bpfland's slices
  shrink with the number of waiting tasks, and its deadline ordering
  favours tasks that slept. kairosnp's fixed 1 ms borrow slice doesn't
  shrink.
- So the trade-off is the same as in E2-final: borrower preemption costs
  5–14% batch throughput against bpfland (about 6% less zstd CPU time from
  the table: 300 − server − idle) and buys a lower median and fewer slow
  requests. The same config's busy batch differs by 4–12% between
  E2-final and this run (kairos 190 vs 212 MB/s periodic, 310 vs 321
  short), so batch gaps under ~10% need same-batch comparisons.

E2a, Azure trace replay (section 6.17):
- The synthetic-pattern results carry over to a real arrival pattern.
  KairOS's median stays at 0.78–0.81 ms at every load level. Every
  baseline's median rises with load: bpfland from 0.98 to 1.28 ms, never3
  from 1.07 to 2.82, Linux from 1.24 to 4.09.
- The trace spends much of its time at moderate load (1000–2000 req/s, a
  third of the requests), which the synthetic patterns hardly had. There
  KairOS has a quarter of bpfland's requests over 2 ms (4.0 vs 15%).
  Linux and lavd already have a 1.5–1.7 ms median there.
- The extreme tail is not better: p99.9 of all requests is ~100–110 ms
  for kairos, bpfland and Linux alike. Guess: the slow episodes seen in
  every config since N1 (section 6.1), not waits for a CPU.
- The batch cost follows the service's load. It is 1% below bpfland in
  the mostly quiet hour 284 and 13% in hour 54 with its busy stretch, 8%
  over the whole replay. The mechanism is the same as in E2k: more
  context switches (39 vs 28 k/s) and 9% of a CPU more idle time on the
  server's CPUs with borrower preemption. The server also used 6% of
  a CPU more.
- The trace is much less busy than the synthetic patterns. Only 5% of the
  time is at 2000 req/s or more, against a third to a half in E2's
  patterns. KairOS's
  batch gap to bpfland (8%) is therefore at the low end of E2-final's
  7–28%.

E3 rerun, burst-length sweep (section 6.19):
- KairOS's latency doesn't depend on how long a burst lasts: its busy
  p50 is 0.90–1.00 ms from the first 2 ms of a burst to 10 s into it,
  and at every L from 10 ms to 10 s. The LC pool follows demand with a
  ~64 ms average, but on 3 CPUs that reaction time doesn't matter.
  Borrower preemption serves each wakeup at once whatever the pool size,
  and the pool was at all 3 CPUs in 97–98% of seconds anyway, quiet
  phases included: 500 req/s needs ~0.5 CPUs, and the 1.5-CPU buffer
  rounds that up to 3. So zstd only ever borrowed, and was preempted
  ~8,500 times a second on average. The expectation at the top of this
  section ("adaptivity helps only when bursts last longer than the
  reaction time") doesn't apply: KairOS's fast path doesn't adapt, it is
  always on.
- The baselines get worse as a burst goes on, then partly recover:
  eevdf's p50 rises from 2.1 ms (0–2 ms) to ~8 ms (30–300 ms) and falls
  to 3.2–4.3 ms after 300 ms; lavd's is similar (2.2 -> 7.3 -> ~5 ms).
  A queue builds at the start of a burst while the batch jobs hold the
  CPUs for whole slices. Why it shrinks again after ~300 ms is not
  known (not investigated; guess: Go's scheduler or the batch jobs'
  vtime/lag settles).
- The backlog also outlives the burst: in the first 2 ms of a quiet
  phase 17–24% of requests take over 5 ms under eevdf, lavd and never3,
  and the share falls to the quiet level within ~100 ms. KairOS and
  bpfland don't carry a backlog (1.6–1.8%), so their quiet phases are
  quiet from the start.
- bpfland is the strong baseline again. It never preempts, yet its
  median is 1.3–1.6 ms at every L, because its waits are bounded by
  short slices (section 2.11). KairOS's advantage over it is in the
  slow requests (busy >5 ms 2–6% vs 9–21%) more than in the median.
- The batch cost is the same at every L (391–397 MB/s against 421–495
  for the baselines) and matches E2-final/E2a. It is CPU time: zstd gets
  ~0.1–0.2 CPUs less (more idle and more server CPU), while MB/s per
  zstd CPU-second is about the same in every config. It isn't the BPF
  callbacks: E7 puts kairos's callbacks at ~1.5% of one CPU more than
  never3's (0.015 CPUs), a tenth of the gap.
- So E3 confirms what E2k and E2a suggested: what makes KairOS fast on
  3 CPUs is preempting batch work on every service wakeup, and its price
  is paid in context switches and lost batch CPU time. That is also
  what Linux's SCHED_IDLE does (section 2.11). Whether KairOS's split
  adds anything over it is the question E3-idle tests.

E7, overhead (section 6.20):
- Where nothing gets preempted, KairOS costs little. With batch jobs only,
  every scheduler is within ±3% of scx_simple, inside the run-to-run
  spread. At light load kairos has the lowest median (1.04 ms against
  1.14 for scx_simple and 1.25 for Linux), probably because an LC task
  takes its previous CPU first (`split_pick_idle()`; guess).
- Callbacks cost more per call than scx_simple's (running() 164 vs
  39 ns, select_cpu() 363 vs 247 ns: task-storage lookups, wait
  accounting, the per-CPU context and the idle-CPU scan), but in total
  4.1% of one CPU under E2's load, less than bpfland (5.5%) and lavd
  (7.3%). Part of the difference is more calls: kairos switches tasks
  more often (preemptions), so running()/stopping() run ~2x as often as
  under scx_simple.
- Sync wakeups are a real gap. When a task wakes another and is about to
  sleep (pipes, the kernel's WF_SYNC), `scx_bpf_select_cpu_dfl()` puts
  the wakee on the waker's CPU if that CPU has nothing queued and the
  system isn't saturated (kernel/sched/ext_idle.c). never3 and
  scx_simple use it and stay at 1.6 µs per pipe round trip on 2 CPUs.
  kairos's `split_pick_idle()` ignores the flag and moves the wakee to
  the other, idle CPU every time: 15.2 µs, like Linux and lavd. On this
  VM waking an idle vCPU seems to cost ~7 µs per hop (guess: the host
  has to wake the vCPU thread). Whether the gRPC path has sync wakeups
  (Go's futex wakeups are not sync; loopback TCP may be) is not
  measured.

E3-idle, Linux with SCHED_IDLE batch jobs (section 6.21):
- KairOS beats Linux's own mechanism for "batch only on spare CPU": at
  about the same batch throughput (2% less over the whole run, within
  this batch's spread), its busy median is 2–3x lower and it has 2–14x
  fewer busy requests over 5 ms, in 3/3 reps at every burst length.
- So the batch cost is a property of yielding to the service on every
  wakeup, not of KairOS's code. Under Linux, SCHED_IDLE costs about as
  much batch throughput as KairOS's borrower preemption, with the same
  context-switch rate (44–45k/s) and the same extra idle time on the
  server's CPUs (0.37–0.38 CPUs, vs ~0.31 with SCHED_BATCH). This weakens
  the guess in 6.16 that the extra idle is a work-conservation gap in
  KairOS's queues. Both implementations show it; the cause is still not
  known.
- Why KairOS gets more latency out of the same displacement (guess, not
  measured): its LC tasks share one queue (LC_DSQ) that any LC CPU takes
  from as soon as it frees up, and a woken LC task picks an idle CPU or a
  borrower from all 3 CPUs. Linux places a woken task on one CPU's run
  queue (`select_idle_sibling()`, which also counts SCHED_IDLE CPUs as
  idle), and after that only load balancing (when a CPU goes idle, and
  periodically) moves it. The 16 Go threads compete with each other in
  per-CPU queues with 2.1 ms base slices. One shared
  queue for 3 servers waits less than 3 separate queues (the M/M/3 vs
  3×M/M/1 effect that Shinjuku and Concord rely on, section 2.2). A run
  of KairOS with per-CPU LC queues would test this.
- What this means for the claim: on 3 CPUs, KairOS's value is not the
  pool sizing (the pool was full 98% of the time) but service-first
  scheduling from a shared queue. It gets 2–3x lower median latency than
  Linux's built-in option for this at the same batch throughput, and ~1.5x
  lower than bpfland at 6–13% less batch.

E10–E12 and G (sections 6.22–6.25):
- Where kairos's preemptions come from. 94–96% of its ~8,500
  preemptions/s hand a CPU to one of the Go server's non-worker threads,
  which runs 7–12 µs and sleeps again. The request workers, whose runs
  are ~0.5 ms, cause 2–5%. For each of these zstd pays a context switch
  and a colder cache to give a thread ~10 µs. Guess (not measured per
  thread): mostly Go's idle Ms waking to look for work ("spinning") and
  the netpoller handing requests to workers.
- Delayed preemption. If such short-running threads wait for a CPU
  instead of preempting, they almost always get one soon without a
  preemption: with D = 1 ms the timer had to preempt for 11 of ~6,200
  delayed wakeups/s. Preemptions fall 90%, context switches 25%, idle on
  CPUs 0–2 0.37 → 0.33 CPUs, and batch rises 6% (5/5 reps), to
  bpfland's level. The cost is +0.25–0.35 ms on the busy median: these
  threads are on the request path (E10: their waits per request rose
  0.1–0.27 ms with D = 250 µs), so delaying them delays requests. The
  tail moves less (busy >5 ms 2.0–7.7% → 3.7–9.5%), since a wait of at
  most 1 ms is small against 5 ms.
- Exempting the workers matters: pw250, where every LC task waits, has
  a higher busy p50 (1.38–1.55 ms) and more busy requests over 5 ms
  (6.0–10.9%) than pw250r100 (1.20–1.25 ms; 2.4–8.9%) at about the same
  batch (395 vs 392 MB/s). Preempting only for tasks whose runs are long
  enough to be worth it is a simple form of the classification idea
  (Feature A, section 3), keyed on the average run length.
- So D is a latency-for-throughput knob, and D = 1 ms with workers
  exempt gives a point that beats bpfland: the same batch (399 vs 394
  MB/s) with a busy median lower in 4–5/5 reps at every burst length
  (1.21–1.45 vs 1.55–1.69 ms) and 2–4x fewer busy requests over 5 ms.
  D = 100 µs keeps kairos's latency (p50 +0.05–0.1 ms), but its batch
  gain is not consistent (2/5 reps).
- Workers preempt more when the short threads wait (E10, busy: 460 →
  1,192/s). Guess: under kairos, a worker woken behind a short thread is
  often given the CPU that thread took by preemption once it sleeps
  ~10 µs later, so the worker gets a CPU without preempting; when the
  short threads wait, a woken worker finds only borrowers.
- The shared LC queue. With per-CPU LC queues, kairos's busy median
  rises from 0.86–1.04 to 1.46–1.71 ms and its busy share over 5 ms
  2–5x, with more system time and more runnable tasks and no batch
  gain. This supports the guess in the E3-idle explanation: much of
  kairos's lead over Linux with SCHED_IDLE (1.99–2.99 ms) comes from one
  shared queue for the service (the M/M/3 vs 3×M/M/1 effect). The rest
  (percpu still has a 1.3–1.8x lower median than eevdfidle) would come
  from the other differences (guess): LC tasks never wait behind batch
  on their CPU, a woken LC task looks at all 3 CPUs, and LC tasks run
  FIFO with 3 ms slices instead of sharing CPUs fairly among 16 Go
  threads.
- Sync wakeups fix the pipe case (2-CPU round trip 14.9 → 1.6 µs,
  simple's 1.5) but the gRPC workload has only ~6 per second (the load
  generator's wakeups come from CPU 3, outside the pool), so they change
  nothing there, as expected.
- Victim choice. `longest` spreads preemptions evenly across CPUs 0–2
  but changes neither latency nor batch measurably with 3 reps; CPU 0
  taking more preemptions costs nothing visible here.
- Redis (G). The colocation result holds for a very different service.
  A GET takes ~10 µs, so Redis needs ~0.1 CPU, but every request is a
  wakeup. Linux (with zstd SCHED_BATCH or SCHED_IDLE) and bpfland raise
  Redis's median from its no-batch 0.1–0.2 ms to 0.5 ms; kairos keeps
  0.1 ms, at 6% less batch than eevdf, twice the system time (19% vs
  10%) and 11k preemptions/s. Delayed preemption (D = 200 µs) halves the
  batch loss (732 vs 707 and 755 MB/s) and keeps a 0.3 ms median. Why
  Linux with SCHED_IDLE batch still adds ~0.3 ms, when a woken Redis
  should preempt a SCHED_IDLE task at once, is not known. The same was
  seen for gRPC in E3-idle (1.8–3.0 ms against ~1 ms).
- Redis used 2.5x the CPU under kairos (0.27 vs 0.11 CPUs; without
  batch 0.37 vs 0.22). Cause not measured. A first guess, fewer requests
  handled per wakeup when Redis is served at once, doesn't explain the
  no-batch case, where every scheduler finds idle CPUs.

ML and the Azure replay (sections 6.13, 6.26):
- The predictor can only raise the demand the pool is sized from, and on
  3 CPUs the buffer already fills the pool, so a better forecast has
  nothing to change. With spare CPUs it could matter: a tighter pool
  would keep some CPUs for batch alone, and a forecast would grow the
  pool before a burst instead of after it (guess, not measured). The
  gain would be bounded by what batch loses on borrowed CPUs, which
  delayed preemption already reduces (E11).
- Its inputs cover the last 500 ms. The training patterns have random
  onsets (`ml/patterns.py random`), and the Azure replay's 10 s bins
  average away the trace's per-minute spikes. What remains at 0.05–0.5 s
  is mostly the current level, which the EWMA already follows, so the
  tree learns a buffer (guess, consistent with the 94–97% of fresh ticks
  in which it raised the demand, 6.13).
- At 1 s (6.27) the per-minute spike is there, and a seasonal mean
  predicts it. But at 1.4–2.2 arrivals per second, most of the change
  from one second to the next is counting noise (a guess, consistent with
  lag-1 autocorrelations of 0.04–0.36), and no predictor can foresee
  noise. The onsets are 3% of samples, so the noise sets how often pools
  are too small.
- The tree is fitted to squared error over all samples. It spends its
  splits on that bulk and uses its seasonal inputs less well than a plain
  seasonal mean (26–37% vs 13–17% under-provisioned at :00, E2a's hours).
- Any gain from forecasting would come from a rule, not a model:
  max(ewma, seasonal mean) was the best simple predictor in 4 of the 15
  test hours. Whether that matters online is not measured.

Duplicate borrower picks and `--claim-borrower` (sections 6.28–6.30):
- **Why duplicates are common under kairos.**
  - A preemption is decided in one CPU's enqueue(). The kicked CPU then
    switches only after an IPI and its scheduling path. Until then it
    still shows `running_at` set and `batch` true.
  - At ~8,000 preemptions/s on 3 CPUs, a second LC wakeup inside that
    window picks the same CPU in ~1 of 5 cases. PICK_FIRST makes it
    likelier, because every wakeup starts its scan at CPU 0 (guess).
  - Under pw1000r100 most LC wakeups don't preempt (~900/s), so the
    window is rarely hit (1–3%).
- **What the fix changes.** Without it, the second task goes to the head
  of the first one's CPU queue (`SCX_ENQ_PREEMPT` implies
  `SCX_ENQ_HEAD`). The first task then waits for it while another LC CPU
  may still run batch work. With the fix:
  - about half of those wakeups preempt that other borrower;
  - the rest wait in LC_DSQ, which every LC CPU serves first.

  Either way, neither task is pushed back on a CPU it can't leave.
- **Why the gain is small** (kairos busy p50 −2%). The duplicates are
  ~1,600 per second (kairos on the burst sweep), against ~33,000 woken
  tasks that start running per second, of any class (`lat_wakeups`).
  Most preempting runs last 7–12 µs (E10), so the task
  that was pushed back lost little. The busy median moves by 20 µs. The
  tail (>5 ms) and batch don't move, because they are set by bursts
  that saturate the LC CPUs, where no free borrower exists anyway.
- **pw1000r100's short-burst pattern is not explained.** The fix
  changes ~40 of its decisions per second. Losing 17 MB/s of batch in
  the L 10 and 30 ms blocks, 4%, would be a large effect for that. With
  21 unadjusted per-L tests, chance is a candidate, though the four
  results point the same way. Another candidate is the timer path: an
  overdue task whose claim fails loses its deadline. That could
  lengthen waits in the short bursts, where the timer fires most
  (guess). Not tested.

GOMAXPROCS 3 (section 6.31):
- **Why EEVDF collapses** (a guess, consistent with the numbers). With 3
  Ps, at most 3 server threads run Go code at once. EEVDF shares CPUs 0–2
  fairly among them and the 2 zstd jobs (SCHED_BATCH has nice 0's
  weight), so the server gets about 3/5 of 3 CPUs, 1.8 CPUs. A busy phase
  needs 3,500 req/s × ~0.61 ms of server CPU per request, 2.1 CPUs. So
  every busy phase is an overload, and its queue grows until the burst
  ends. With 16 Ps, the server's threads are 16 of 18 runnable, and
  fair sharing gives it nearly all 3 CPUs. E1b chose 16 so that the
  kernel sees the server's threads (section 2). This run shows how much
  that choice matters for EEVDF: GOMAXPROCS is a strong lever for the
  baseline, not only for kairos.
- **Why kairos doesn't care.** LC threads have strict priority on LC
  CPUs, and batch only borrows. The number of server threads changes
  how many of them can be runnable, not whether they beat zstd to a CPU.
- **bpfland copes** (1.56 ms), presumably because it favours tasks that
  sleep often (interactive) over CPU-bound ones (guess, not checked in
  its source).

**F (6.32): what the delay buys.**
- **The trade-off.** kairos (D = 0) and pw1000r100 (D = 1 ms) are two
  points on one latency–throughput trade-off:
  - pw1000r100 has 10% more batch than kairos on the burst sweep (7% on
    the Azure replay);
  - it has 29% (16%) higher busy p50.
- **Where pw1000r100 lands against bpfland.** At D = 1 ms it reaches
  bpfland's batch throughput. Neither workload shows a difference: −0
  and −3 MB/s.
- **How much of kairos's lead it keeps** (from the config medians, not
  paired):
  - On the burst sweep: 0.17 of the 0.48 ms between kairos's and
    bpfland's busy p50 (35%), but 7.0 of the 8.6 points of busy share
    over 5 ms (81%).
  - On the Azure replay: 0.12 of 0.27 ms of p50 (44%) and 1.1 of 1.8
    points over 5 ms (61%).
- **Why the delay costs more at the median than in the tail** (a guess
  from the design, not measured per request):
  - A woken LC task that finds no free LC CPU waits up to D before it
    preempts a borrower. That adds up to 1 ms to many requests, which
    moves the median.
  - The wait is bounded, so it rarely pushes a request past 5 ms. The
    tail comes mostly from waits that D doesn't touch.
- **Why the LC pool never shrinks in F** (from update_split(); the
  demand numbers are from 6.32):
  - The pool is ceil(demand + buffer) CPUs. It shrinks only if it would
    also with 0.25 CPU more demand (`--lc-hyst-pct`).
  - With `--lc-buffer-pct=150` (1.5 CPUs), going from 3 CPUs to 2 needs
    a demand of at most 0.25 CPU.
  - kairos-r1's demand rarely fell below 0.70 CPU (1 s p10), so the pool
    stayed at 3. The demand counts every LC-class task: the server, the
    load generator (about 27% of a CPU) and, in F, my wait process.
  - So in F's main configs, kairos's split was a fixed 3-CPU
    reservation. Their gains over the baselines come from the
    reservation, strict LC priority and how borrowers are preempted.
    They don't come from sizing the pool to the load. With this buffer,
    F doesn't test pool sizing.

---

## 8. Limitations
- VM: vCPUs are macOS threads on mixed P/E cores; no SMT/NUMA topology; fanless
  thermal throttling.
- Synthetic service (gRPC CPU loop) may not reflect real request costs; mitigated
  with Redis runs.
- Trace replay is time-compressed and rate-scaled.
  → Details (E2a, 2026-11-01):
  - 10 s bins at 100x compression average away structure shorter than
    10 s of trace, such as the spikes at the top of each minute from
    timer-triggered functions (5.7% of all arrivals fall in second :00).
  - Each window is scaled to its own busiest bin, so the windows' absolute
    load levels are not kept.
  - Within a bin, arrivals are Poisson, not the trace's own timing.
  - Three hours, not the whole two weeks.
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
- Run-to-run noise changes over the day: tails in the evening of
  2026-10-30 were 2–8x those of the afternoon for the same workload,
  Linux's scheduler included (section 6.10). Compare tails only within
  one batch of interleaved runs.
- 3 CPUs is far fewer than a production server; with more CPUs an idle
  one is more often free, so preemption decisions matter less often.

- Host-side slowdowns (the Mac taking CPU from the VM) don't show in the
  in-VM noise check: the guest just sees everything take more CPU time.
  The sign is CPU time per request rising for the server and the load
  generator together (E2-final short rep 5, section 6.14). Such runs are
  left out by whole reps.
- Detecting them (2026-10-31): /proc/stat's CPU times advance only while
  the VM's CPUs run (tick-based; no steal time is reported here), while
  per-process CPU times come from a clock that keeps going while the host
  has paused a vCPU, so a task caught on a paused vCPU is charged wall
  time. The VM time share (/proc/stat time over wall time between the
  run's snapshots) is ~0.97–0.98 on a quiet host; `summarize_run.py` warns
  below 0.95 and `compare.py` prints it. run.sh now calibrates three
  times, keeps the fastest and refuses to start below `ITERS_MIN` (950).
- Correction to the "other processes" figures before 2026-10-31: kworkers
  rename themselves after the work item they run, and the check matched
  processes by name, so a renamed kworker was charged its whole lifetime
  CPU (e.g. "kworker/0:0 10–12%" in E2-final, really ~0). Now matched by
  pid and start time; E2-final's runs were mostly at 3–10% of a CPU
  (ten at 10–24%, one at 40%). Earlier figures (E2s 9–21%, MLte 12–18%, ...) are overstated by
  an unknown amount.
- The VM time share's normal value depends on the scheduler loaded. Linux
  reads 1.00–1.01 in every experiment (E2-final, E2a). sched_ext
  schedulers read 0.97–0.98, lavd 0.96–0.97, on a quiet host. The cut at
  0.95 therefore leaves less margin under sched_ext. Not investigated.
  Guess: /proc/stat's busy times are sampled at ticks while idle time is
  measured exactly, and the sum falls short when CPUs switch between
  idle and busy more often.
- This Claude session and the VS Code window it runs in add load inside
  the VM while it writes messages: 10–20% of a CPU during the run in
  which it happens (E2k, E2a). During runs it should stay idle; a watcher
  that only reports bad runs keeps it quiet.
- Memory. The VM has 3.9 GB, and VS Code's server and this Claude
  session hold ~1.1 GB of it after a long session. After 4 days of uptime
  the VM was swapping during runs, and its worst seconds came with
  swap-in bursts (E3, section 6.18). Each run now records /proc/vmstat
  before and after (`vmstat_before`, `vmstat_after`). summarize_run.py
  warns above 1,000 pages swapped in (a first guess). compare.py prints
  swap-ins and direct-reclaim stalls per config.
- Waking an idle vCPU is expensive here. A pipe round trip across 2 CPUs
  costs ~15 µs under schedulers that wake the other CPU, against 1.6 µs
  when both tasks stay on one (E7, section 6.20), so ~7 µs per wakeup of
  an idle CPU (guess: the host has to run the vCPU thread again). On bare
  metal this is typically a few µs or less (not measured here), so
  placement choices that wake idle CPUs cost more in this VM.
- Any CPU-heavy process during a run disturbs it, even niced and pinned
  to the load generator's CPU 3. An analysis at ~90% of a CPU there cut
  zstd's throughput on CPUs 0–2 by ~20–25% in two E7 runs (section
  6.20). Analyses only between batches.
- Three CPUs for service and batch. In E3 KairOS's LC pool was at all 3
  CPUs in 97–98% of seconds, so pool sizing (reactive or predicted) was
  never exercised. A machine with spare CPUs is needed to test it.
- Memory after days of uptime. After 5 days up, E10 and E11 had steady
  direct-reclaim and compaction stalls (in E10 about 50–80 and 150–250
  per second) without swapping, and they faded during the night (E12
  almost none, G none). They fell to almost none while batch was
  starved in E10's kpw-r2, so they seem to come with zstd's allocations
  (guess). kairos's E10 and E11 numbers match E3-r and E3-idle, so I
  kept the runs. Restarting the VM before a long batch should avoid
  this (not tested).
- The load generator limits Redis tails. At 20,000 GETs/s on its one
  CPU its send lag p99 is 1–8 ms (G, section 6.25), so G's p99 and p99.9
  are not usable; only medians are. More CPUs for the load generator, or
  a leaner client, would be needed for tail results with Redis.
- Kernel change. E1 to G-nb ran on 7.0.0-38-generic; F will run on
  7.0.0-42-generic, which unattended upgrades installed and the
  2026-11-04 restart booted (section 4).
  - F's comparisons are within one kernel. Comparing F's numbers with
    earlier sections crosses kernels.
  - apt-daily-upgrade runs each morning around 06:10–06:45, so a batch
    still running then can overlap with package installs. The effect is
    not measured.
- Daytime host slowdowns. In DS (13:16–13:24) the host took enough from
  the VM to raise CPU time per request by 35–65% (section 6.28), with a
  VM share of only 0.94–0.95. Runs need the Mac idle, plugged in and out
  of Low Power Mode (section 5.1, Method), not only the VM idle.
- The offline check at 1 s (6.27) has four limits:
  - It predicts the replayed rate, not the demand the scheduler measures,
    so it has no measurement noise and no queueing.
  - It converts rate to demand with a rough 1 ms per request.
  - It scores pools by how often they are too small, not by latency or
    batch throughput.
  - Its load is light (mean 0.35–0.38 CPUs), because each hour is scaled
    to its busiest second.
- E14 (6.30) adopted `--claim-borrower` because it was "not worse at
  p < 0.05". With 9–10 reps that test can miss small costs. pwclaim's
  burst-sweep tail and batch lean worse (p = 0.065), so F's pw1000r100
  may carry a cost of a few percent at short bursts that E14 couldn't
  show.
- F's kairos, pw1000r100 and fixed3 are not the configs of the same
  names in E3, E11 and E12: they add `--claim-borrower`. They also run on
  kernel 7.0.0-42 instead of -38. Comparisons of F with those
  experiments cross both changes. Batch was 7–9% higher in E14 than in
  E11 for the same configs without the fix.
- GMP3 (6.31) has 6 reps, too few for Holm to reject over 3
  comparisons. Its result is the 6/6 direction, not a test.
- The apt timers can't be planned around.
  - At 18:06, `systemctl list-timers` showed apt-daily's next run at
    23:40. It ran at 18:14:15 instead (journal), during DS2's fixed3-r1:
    0.9 s of CPU, 92 MB peak memory.
  - The timer is `OnCalendar=6,18:00` with `RandomizedDelaySec=12h`.
  - `other_cpu()` misses processes that start and exit between the
    snapshots, so the run checks can't see such a job.
  - It was small here. apt-daily-upgrade, which can install packages, is
    the larger risk. Stopping both timers during F is the reliable way.
- **F's absolute numbers come from a slower night than E14's** (6.32).
  - All night, the server needed about 10% more CPU per request, and
    batch was 11–19% lower for the same configs.
  - The run checks don't catch this: VM share was 0.97, and calibration
    at the start of each part read 993–1,037 iterations/µs.
  - Possible causes, guesses not separated by any measurement:
    - my wait process (next item);
    - the host giving the VM slower or busier cores (the P/E-core and
      throttling limits above).
  - **Relative results match E14 where they overlap.** pw1000r100's busy
    p50 is 29% above kairos's in both (E14 without the fix: 1.20 vs 0.93
    ms). Its batch is 10% above kairos's (E14: 8%).
  - **Rerun?** The pre-set rules don't call for one. Whether to rerun F
    on a quieter night is Aayush's decision.
- **My wait process ran during every F run.**
  - **What it was.** `tail --pid=<wrapper> -f /dev/null` (uutils
    coreutils 0.10.0), started from the Claude Code session to report
    when F ended.
  - **Its CPU differed by scheduler,** so it isn't a neutral load:
    3.5–4.5% of a CPU under EEVDF, 5.6–8.3% under the sched_ext
    schedulers (config medians).
  - **Its class under kairos.** It is SCHED_NORMAL, so under `--class=policy`
    presumably LC (not checked).
  - **Why it cost so much is unknown.** In a 10 s test with the machine
    idle it used 0.1% of a CPU.
  - **Next time:** no extra process during runs, or a loop that checks
    once a minute.
- **fixed3 doesn't test pool sizing on the burst sweep.** kairos's LC pool
  held 3 CPUs in every 1 s sample, so fixed3 ran the same pool (6.32). F
  has no pool-sizing ablation, and the Azure part has no fixed3.

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
  → E7 (section 6.20): kairos's select_cpu() takes 363 ns per call
  (its own idle scan), scx_simple's 247 ns (the kernel's search); all
  callbacks together 4.1% of a CPU. Skipping the search not tried.
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
- (E2k rerun, section 6.16.) Why does borrower preemption leave the
  server's CPUs idle 6–8% of a CPU more, mostly CPU 0, while batch gets
  less CPU time? Idle kicks don't change it. Next check: count, per CPU,
  time idle while LC_DSQ or BATCH_DSQ holds a task (`ops.update_idle`),
  and preemptions per CPU.
  → E3-idle (section 6.21): Linux with SCHED_IDLE batch jobs shows the
  same extra idle (0.38 CPUs, vs 0.31 with SCHED_BATCH), so it comes
  with preempting batch work on wakeups, not only with KairOS's queues.
  The per-CPU counts are still to do.
- (E3-r, E7, E3-idle; improvement directions, in order.) (1) Which
  wakeups cause kairos's ~8,500 preemptions/s, and how long does the
  preempting task then run? Guess: many are Go runtime threads that run
  briefly. (2) Does preempting a borrower only after a woken LC task has
  waited some time D keep most of the latency at less batch cost (a
  latency-for-throughput knob)? (3) Do sync wakeups matter beyond the
  pipe benchmark (Redis)? (4) Is the shared LC queue why kairos beats
  SCHED_IDLE (per-CPU LC queues as an ablation)? (5) Does the victim
  choice (CPU 0 first now) matter? (6) On a machine with spare CPUs,
  does pool sizing matter?
  → E10–E12, G (sections 6.22–6.25): (1) the server's non-worker
  threads, 94–96% of preempting runs, 7–12 µs each; (2) yes: D = 1 ms
  with the workers preempting at once gives bpfland's batch at a lower
  latency than bpfland, +0.25–0.35 ms on kairos's busy median; (3) not
  for gRPC (6 sync wakeups/s); they fix the pipe test; not tried with
  Redis; (4) yes, in large part; (5) no visible effect; (6) still open.
- (E11, G.) Why does Linux with SCHED_IDLE batch jobs still raise the
  service's median (Redis 0.1–0.2 → 0.5 ms, gRPC ~1 → 1.8–3.0 ms), when
  a woken service task should preempt a SCHED_IDLE task at once? Which
  hop waits (the wakeup, softirq or ksoftirqd, the reply's path)? Next
  check: trace one request's wakeups under eevdfidle.
- (G.) Why does Redis use 2.5x the CPU under kairos, also without batch?
  Next check: perf stat on redis-server (cycles, syscalls per request)
  under eevdf and kairos.
- (E10.) Do workers preempt more with delayed preemption because they
  lose the CPUs the short threads used to open for them (guess in
  section 7)?
- (ML, 2026-11-04.) Can a predictor beat the EWMA plus a fixed buffer
  when (a) there are CPUs to spare beyond the service's peak, so a
  smaller pool frees CPUs for batch alone, and (b) burst onsets follow
  from history? Candidate pattern: the Azure trace at 1 s bins (10x
  compression), where the timer-triggered spikes at second :00 come every
  6 s of replay. It must also beat a seasonal baseline (demand one period
  ago). Next check: offline first (`--ml-trace` on that pattern,
  `ml/train_burst.py`), then a VM with more vCPUs or a cloud ARM64
  machine.
  → Offline on the replayed rates (6.27): at equal cost the tree did not
  beat the simple predictors. It was lower than the best one in 0/3 of
  E2a's hours and 2/12 others, by ≤ 0.3 points. So there is no run with
  more CPUs: Aayush's order makes that run conditional on the model
  winning offline. Still open: whether a seasonal rule (max(ewma,
  seasonal mean), no learning) helps online on a machine with spare CPUs.

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
**Did:** Studied background papers; surveyed sched_ext schedulers (bpfland, flash,
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
whether the idle-CPU search is wasted work under high load. Trimmed the
`scx_kairos.bpf.c` header comment.
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

### 2026-10-30 — Phase 5: adaptive switching against a latency target
**Did:** Revised the mode decision (section 3.4) to a latency target per
the user's choice. Implemented `--adaptive` (late share of woken tasks,
fast exit to LATENCY, hold with doubling before BATCH; signal CPUs). Ran
bursty pilots with 2 and 1 zstd jobs; fixed the smoothing window and the
fixed-point floor.
**Found:** Section 6.8. Works mechanically with 1 zstd job; with 2 the
target is never met in BATCH mode, so it stays in LATENCY.
**Interpretation:** In these workloads the mode switch has little room on
top of batch-only preemption (guess until E2 with reps).
**Decisions:** Defaults TARGET 1 ms, LATE_MAX 5%, ~64 ms smoothing, hold
200 ms–5 s; 2^20 fixed point for the late share.
**Problems:** x1024 EWMA floor (6.2%) kept the mode in LATENCY; a `grep`
on stdout of a stderr-only `--help` stopped one command chain early.
Pilots ran with the IDE in use.
**Next:** E2 with reps (never, never3, batchvic, adaptive, EEVDF) on a
bursty pattern, IDE idle; decide which workload gives the switch room
(e.g. a looser target or costlier batch preemption).

### 2026-10-30 — Experiment data kept out of git; neutral wording for publishing
**Did:** Added `results/` to `.gitignore`. (First committed it without the
per-request logs, 50 MB; undid that unpushed commit at the user's request.)
Reworded the course references in CLAUDE.md and this log (header, 2.8
heading, two session entries) before publishing the repo.
**Found:** 2,779 untracked files, 1.8 GB; `requests.csv` is 1.7 GB of it.
**Interpretation:** None.
**Decisions:** Experiment data stays only in the VM; the log's tables and
the `results/` paths it cites are the public record.
**Problems:** Old commits still contain the previous wording.
**Next:** User pushes; then E2 with reps.

### 2026-10-30 — E2: bursty load, 10 reps
**Did:** Pilot (E2pt) of 5 and 10 ms targets with 2 zstd jobs, then E2
with 10 interleaved reps, 1 zstd (eevdf, never, never3, batchvic,
adaptive 1 ms) and 2 zstd (same, with a5 and a10), IDE idle.
**Found:** Section 6.9. No failed requests. 1 zstd: adaptive ≈ batchvic,
quiet batch +0–2.5%. 2 zstd: a10 never leaves BATCH; a5 lands between
never and batchvic with worse busy p99 than batchvic and never3.
**Interpretation:** Section 7. The late share of wakeups misses most of
what makes requests slow, so the target is a knob, not a bound. The
mode switch adds little on top of batch-only victims here.
**Decisions:** None yet; next step needs the user's choice.
**Problems:** Three runs with other processes above 30% of a CPU (VS
Code server) despite the idle IDE.
**Next:** Either measure the waits the signal misses (non-wakeup
enqueues of server threads) and fix the signal, or go back to a
load-based switch, or keep batchvic as the main mechanism and move on to
Feature A, the baselines (bpfland, lavd) and overhead (E7).

### 2026-10-30 — What the latency signal misses; time slices
**Did:** Added `--diag` (waits per task group and reason) to scx_kairos,
named the server's worker threads, logged Go's goroutine waits, and had
the server report handler receive time and wake delay. Ran ED (3 reps),
ES (2 reps) and a control without diagnostics (EC). Read the slice rules
of EEVDF, CFS, bpfland, lavd and related papers (section 2.9).
**Found:** Section 6.10. Server-thread waits are ≥ 95% wakeups. Workers
wait 2–5 ms after wakeups in BATCH mode. Request tails sit in the
gRPC/network path and the client. Evening runs were 2–8x noisier.
**Interpretation:** Section 7. The signal sees the right waits but a
fixed per-wakeup target can't map them to request latency; slice length
bounds the waits directly.
**Decisions:** None yet (user to choose the next step).
**Problems:** The first test of a server's thread names matched my own
shell with `pgrep -f` and killed it; used `$!` instead.
**Next:** Options: a flexible slice in BATCH mode; a signal from total
waiting time of the service's threads (or the service's own latency);
keeping the load generator out of the scheduler under test.

### 2026-10-30 — Research: how colocation systems get both
**Did:** Read how Heracles, PerfIso, Shenango, Caladan, Meta's ads
sched_ext policy, scx_layered, scx_lavd, scx_bpfland, Linux SCHED_BATCH
and Tales of the Tail handle latency-critical plus batch work (section
2.10). No code changes.
**Found:** Most deployed systems split CPUs between the two classes and
resize the split by load or by the service's latency, rather than
deciding per wakeup whether to preempt on shared CPUs. Where they share,
they shrink slices instead of preempting at once.
**Interpretation:** Our design time-shares 3 CPUs per wakeup, the hardest
version of the problem; the measured trade-off in E2 (preempt: zstd
interrupted ~3,600/s; don't: 2–5 ms waits) is what CPU splitting avoids.
**Decisions:** None; the user chooses the direction.
**Problems:** Several numbers come from abstracts and summaries only.
**Next:** Choose among a soft CPU split sized by load or queueing delay,
a flexible slice, and a service-side or queueing-delay signal.

### 2026-10-30 — Soft CPU split: implementation and pilots
**Did:** Implemented `--split` (LC and batch CPU pools, own queue first,
lending, LC wakeups preempt borrowers, pool sized by LC demand with
hysteresis), `--class policy` (batch = SCHED_BATCH) and the harness's
`BATCH_POLICY`. Pilots S1p (1 rep) and S2p (2 reps, GOMAXPROCS 16 and 3,
with a no-borrower-preemption variant).
**Found:** Section 6.11. Noise from other processes (3–137% of a CPU)
dominated the tails. Linux with zstd as SCHED_BATCH failed requests in
every run.
**Interpretation:** Section 7. Promising but unproven at GOMAXPROCS 16;
borrower preemption is what gives the latency.
**Decisions:** Keep borrower preemption on by default.
**Problems:** Verifier rejected an unbounded rodata index (fixed with
`MEMBER_VPTR`); the pool flapped with an 8 ms average (now 64 ms +
hysteresis); memory pressure (kswapd) and the IDE during runs.
**Next:** A clean run (IDE closed or idle, start delayed so the Claude CLI
is idle): eevdf, never3, batchvicp, split, 5 reps, GOMAXPROCS 16, zstd
SCHED_BATCH; and a check of eevdf with zstd SCHED_OTHER vs SCHED_BATCH.

### 2026-10-30 — E2s: soft CPU split, clean run
**Did:** Ran E2s (eevdf, never3, batchvicp, split; 5 reps; zstd
SCHED_BATCH; Mac idle; 90 s start delay) and E2s-n (eevdf with zstd
SCHED_OTHER, 5 reps).
**Found:** Section 6.12. Quiet run (other processes 9–21%), no failures.
split ≈ batchvicp on latency with slightly more batch throughput and
fewer preemptions; vs never3, much lower median latency for 5% (quiet)
to 30% (busy) less batch throughput. Linux unaffected by SCHED_BATCH.
**Interpretation:** Section 7. On 3 CPUs the split can't keep a spare CPU
when busy, so it reduces to service priority plus borrower preemption.
**Decisions:** None; the user decides whether to keep the split as the
design and how to continue.
**Problems:** None in the run.
**Next:** Options: test the split with more CPUs (VM with 6–8 vCPUs);
cut borrower preemptions (deferred preemption / short borrow slices);
use ML burst prediction to grow the LC pool ahead of bursts.

### 2026-10-30 — ML burst prediction for the soft CPU split
**Did:** Added LC demand sampling, `--ml-trace`, `--ml-model` (tree
evaluated in the loader every 50 ms; prediction can only raise the pool's
demand, ignored when stale); `ml/patterns.py`; `ml/train_burst.py`
(plain-Python CART, persistence / mean / EWMA baselines, matched-buffer
baselines); `analysis/byrate.py`. Collected training data (random
patterns), trained burst-v1, tested offline and online on short-burst and
periodic patterns (5 reps) against the reactive split, a 1.5 CPU buffer
and batchvicp.
**Found:** Section 6.13. The model is equivalent to a bigger buffer
offline and online; a full LC pool costs no batch throughput on 3 CPUs.
**Interpretation:** Section 7. On this testbed, pool sizing and
prediction have no room; service priority with lending is what helps.
**Decisions:** ML inference in the C loader (section 3.8 revised).
**Problems:** Slow episodes in single reps of every config make busy
tails unrankable.
**Next:** User to choose: evaluation phase (baselines bpfland/lavd,
overhead E7, Redis) with the design as it is; a larger-CPU test where
sizing could matter; a periodicity feature for the predictor.

### 2026-10-31 — E2-final: main comparison against the baselines
**Did:** Added baseline scx schedulers to the harness (`scx:NAME[:opts]`,
93d2647). Ran E2-final: eevdf, bpfland, lavd (autopilot and
performance), never3, always, batchvicp and kairos (split, full LC
pool), 5 reps, periodic and short-burst patterns. Analysis additions
(10fb5f5): `--skip-reps`, the share of requests over 2/5/10/50 ms, busy
p90/p95, server CPU and idle time per config. Added idle kicks to
scx_kairos (7f1a122, `--no-kick-idle` to turn off) and E2k configs.
**Found:** Section 6.14. kairos has the lowest busy p50 (0.9–1.0 ms) and
the fewest busy requests over 2/5/10 ms of all configs. bpfland is next
(1.4 ms) with 7–9% (quiet) / 13–28% (busy) more batch throughput. Linux
and lavd: 5–8 ms busy medians. The extra idle time on the server's CPUs
comes with preemption. Three runs at the end of the short batch were
slowed by the host and were left out by rep.
**Interpretation:** Section 7. Preemption buys the lowest latency;
bpfland's short slices with no preemption get close at no batch cost.
Part of KairOS's batch loss looks like a work-conservation gap (no idle
kicks), which the new code addresses.
**Decisions:** Idle kicks on by default. Leave out whole reps when the
host slows the VM (`--skip-reps`).
**Problems:** Host slowdown in short rep 5, invisible to the in-VM noise
check.
**Next:** E2k (bpfland, kairos without kicks, kairos with kicks, kairos
with kicks + no borrower preemption + 1 ms borrow slices; both patterns,
5 reps). Then the Azure trace replay, E3, E7 and Redis.

### 2026-10-31 — E2k invalid; host slowdown checks
**Did:** Ran E2k (bpfland, kairos with and without idle kicks, kairos
with kicks and no borrower preemption; both patterns, 5 reps). Found it
invalid and why; fixed the noise check (match processes by pid + start
time), added the VM time share per run and a calibration guard in run.sh.
**Found:** Section 6.15. The host held the VM back for most of the run (the
Mac busy, then asleep ~30 min, then a VS Code server started on the VM),
and both batches calibrated the per-request work while slowed. The kworker
"noise" in earlier summaries was an accounting artifact.
**Interpretation:** Nothing about the schedulers. In the two clean reps the
idle kicks fire thousands of times a second but did not visibly raise
batch throughput (lighter load; a hint only).
**Decisions:** Screen every run by VM time share (< 0.95 = leave the rep
out); refuse to start when calibration is low.
**Problems:** Runs depend on the Mac staying awake and idle, which the VM
can't control.
**Next:** Rerun E2k with the Mac awake (plugged in, `caffeinate`) and
idle, IDE disconnected.

### 2026-11-01 — E2k rerun: idle kicks off, borrower preemption confirmed
**Did:** Reran E2k with the Mac plugged in under `caffeinate` and the IDE
idle, in a tmux session, with a watcher flagging runs by VM time share.
Made idle kicks opt-in (`--kick-idle`; `--no-kick-idle` still accepted).
**Found:** Section 6.16. All 40 runs clean (VM time share 0.97–0.98,
calibration 1026–1036). KairOS's busy median 0.87–0.94 ms vs bpfland's
1.42–1.49, with 3–5x fewer busy requests over 5 ms. bpfland has 5–6% /
8–14% more batch (quiet / busy). Kicks fire ~3,800/s and change neither
batch nor total idle time. KairOS without borrower preemption (1 ms
borrow slices) has bpfland's batch but a 2.2–2.4 ms busy median.
**Interpretation:** Section 7. The extra idle time comes with borrower
preemption, not with missed wakeups. Preempting borrowers is what gives
KairOS its latency lead, and it costs the batch gap.
**Decisions:** Idle kicks off by default (no gain, slight p50 cost on
short bursts). This changes the 2026-10-31 decision. "kairos" in later
experiments = `--split --split-cpus 0-2 --class policy --lc-buffer-pct 150`,
the E2-final config.
**Problems:** None in this run. This Claude session's own CPU use showed up
as noise (17% in one run) while it handled notifications; the watcher now
only reports bad runs.
**Next:** Azure trace replay, then E3 (burst length), E7 (overhead), Redis.
Optional: per-CPU idle-with-queued-work counter for the batch gap (section 9).

### 2026-11-01 — E2a: Azure Functions trace replay
**Did:** Wrote `workloads/traces/azure_phases.py`, which turns the Azure
Functions 2021 trace into load-generator phases. Three random hours
(seed 1), 10 s → 100 ms, scaled per hour to a 3500 req/s peak with a
300 floor. Ran E2a (eevdf, bpfland, lavd, lavdperf, never3, kairos;
5 reps; one kairos smoke run first in `results/2026-11-01-e2a-smoke/`).
Added `analysis/bylevel.py`, which reports latency by target-rate level
and by trace window.
**Found:** Section 6.17. kairos has the lowest p50 at every load level
(0.78–0.81 ms) and the fewest requests over 2/5/10 ms, in every rep
against every baseline. bpfland is next: p50 0.98–1.28 ms, 3.7% of
requests over 5 ms vs kairos's 2.1%. kairos has 8% less batch than
bpfland and 11% less than never3: 1% in the quiet hour, 13% in the
busy one. p99.9 ~100 ms for kairos, bpfland and Linux alike.
**Interpretation:** Section 7. The synthetic results hold on a real
arrival pattern. Its moderate-load stretches show the gap in tail shares
more than the median. The batch cost follows the service's load.
**Decisions:** Report all 5 reps (leaving out the two partly disturbed
reps changes nothing). Under sched_ext the VM time share cut (0.95) has
2–3 points of margin, not 5 (section 8).
**Problems:** Claude's and VS Code's own CPU use disturbed three runs.
**Next:** E3 (burst-length sweep), E7 (overhead: bpf_stats, zstd-only
throughput, `perf bench sched pipe`), Redis. Optional: a second trace
sample (another seed) to check that the three hours are not special.

### 2026-11-02 — E3 invalid: the VM was swapping
**Did:** Wrote `workloads/traces/burst_sweep.py` (busy and quiet phases of
equal length L from 10 ms to 10 s, one shuffled block per L) and
`analysis/byburst.py` (latency by burst length and by time since a burst
started or ended). Ran E3 (eevdf, bpfland, lavd, never3, kairos; 5 reps)
and a diagnostic eevdf run with 1 s memory counters. run.sh now saves
/proc/vmstat around every run, and summarize_run.py and compare.py report
swapping.
**Found:** Section 6.18. Seconds with at least 10% of requests over 50 ms
were 10–38% of the run time, against 0–6% in yesterday's runs, for every
config. CPU speed was unchanged. The VM was swapping (600 MB in swap after
4 days up), and the worst seconds in the diagnostic run came with
swap-in bursts of 190–940 pages a second.
**Interpretation:** The run measures the VM's memory pressure more than the
schedulers. The configs were interleaved, so all were exposed, but the
episodes dominate p95 and p99 and add noise to the medians.
**Decisions:** E3 is invalid and will be rerun with the VM's memory freed.
Every run now records swapping; leave out runs that swapped. The
threshold, 1,000 pages, is a guess to refine.
**Problems:** `--lc-shift` above ~8 is too coarse for the LC demand
(section 4), so a slow-pool variant of kairos was not tried.
**Next:** Free the VM's memory (restart the VM, or swapoff/swapon), then
rerun E3. Then E7 and Redis.

### 2026-11-02 — E3 rerun, E7 overhead, E3-idle; similar systems
**Did:** Freed the VM's memory without root instead of restarting it: the
page cache was dropped before the batch and each run's files after it
(`scripts/evict_cache.py`). Reran E3 (5 reps). Ran E7: batch-only and
light-load configs, `perf bench sched pipe` on 1 and 2 CPUs, and the BPF
callback cost with `kernel.bpf_stats_enabled=1` (reset to 0 after);
`analysis/overhead.py` summarizes it. Read about systems close to the
split (section 2.11). run.sh can now run the batch jobs as SCHED_IDLE
(`BATCH_POLICY=idle`, per entry `BATCH_POLICY_<label>`), and E3-idle
compared Linux with SCHED_IDLE zstd against kairos (3 reps).
**Found:** Sections 6.19–6.21. kairos's busy p50 is 0.90–0.98 ms at
every burst length from 10 ms to 10 s, lower than every baseline in 5/5
paired, at 6–20% less batch throughput. Its LC pool was at 3 CPUs in
97–98% of seconds, with ~8,500 preemptions/s. Where nothing is preempted
the overhead is small (batch only within ±3% of scx_simple; callbacks
4.1% of a CPU), but kairos ignores sync wakeups (pipe on 2 CPUs: 15.2
vs 1.6 µs per round trip). Linux with SCHED_IDLE batch jobs gets about
kairos's batch throughput (+2%) at 2–3x its busy median.
**Interpretation:** Section 7. The batch cost comes with preempting batch
work on every service wakeup, in Linux too. On 3 CPUs KairOS's value is
service-first scheduling from one shared queue (guess, not tested), not
pool sizing.
**Decisions:** Rep 1 of E7's batch-only runs is left out (my analysis
disturbed it); analyses run only between batches. Linux with SCHED_IDLE
batch jobs (eevdfidle) joins the baselines.
**Problems:** 24 of 25 E3-r runs recorded `git_dirty` (the inactive
BPF_STATS hook in run.sh). pdftotext isn't installed, so paper bodies
weren't read (abstracts and web pages instead).
**Next:** The improvement directions in section 9, in order: what
triggers the preemptions, delayed preemption, sync wakeups, per-CPU LC
queues, victim choice. Then Redis, and a machine with more CPUs.

### 2026-11-03 — Improvement directions implemented; E10–E12 and G ready
**Did:** Merged the E3-r, E7 and E3-idle write-up into this log (8b574e8).
Implemented the directions from section 9 in scx_kairos, all off by
default (section 4): counters for which tasks preempt and how long they
then run, preemptions per CPU and idle CPUs while work waits; delayed
borrower preemption (`--preempt-wait-us`, `--preempt-min-run-us`); the
kernel's rule for sync wakeups (`--wake-sync`); `--borrower-pick`; per-CPU
LC queues (`--lc-queue percpu`). Added a Redis mode to the load generator
and run.sh for G. Wrote configs E10 (diag), E11 (delay sweep against
bpfland and SCHED_IDLE), E12 (ablations), G (Redis) and
`experiments/e10_g.sh`, which runs them in a row (~3.5 h). Every option
set in the configs loads; each feature ran in a 10 s smoke test.
**Found:** Smoke tests only (one 10 s run per config, Claude's session
using 10–15% of a CPU, not measurements): 94–97% of kairos's preempting
runs came from the server's non-worker threads ("grpcserver"), mean ~11
µs, against ~3% from the request workers ("grpcworker", ~0.5 ms); idle
split CPUs while a task waited were under 0.01 CPUs; preemptions fell
mostly on CPU 0 with the default borrower choice. Delayed preemption
(250 µs, workers at once) cut preemptions from ~9,100 to ~1,200/s in the
busy phase. `--wake-sync` took the 2-CPU pipe round trip from 13.3 to
1.8 µs. In a Redis smoke run kairos preempted 18k times/s in the busy
phase.
**Interpretation:** If E10 confirms the split, most of kairos's
preemptions buy little: a Go runtime thread runs ~10 µs and goes back to
sleep, and zstd pays two context switches. Delaying exactly those is the
cheapest fix to try (E11). The idle-while-queued sample suggests the
extra idle time is not a work-conservation gap (guess, from smoke runs).
**Decisions:** New options default off, so earlier configs keep their
behaviour. Redis requests are 1 KB GETs at up to 20k/s, the most the
load generator sends on CPU 3 without falling behind, with 3 zstd jobs
so a woken Redis finds no idle CPU.
**Problems:** The verifier rejected three first versions (nested loops,
an exact counter, a sum in a loop; section 4). One `--borrower-pick prev`
smoke run had a 4 s episode with the server on all 3 CPUs and an 861 ms
longest wait; it looked like the usual slow episodes, but E12 should
check. The VM has 603 MB in swap after 4 days up; e10_g.sh drops the
page cache before each part.
**Next:** Run `experiments/e10_g.sh` with the machine idle, then analyze
E10–E12 and G (compare.py, byburst.py --ref kairos, diag.py for E10,
overhead.py --pipe for the pipe test).

### 2026-11-04 — E10–E12, pipe test, G and G-nb
**Did:** Ran `experiments/e10_g.sh` overnight with the Mac plugged in
under `caffeinate` and the IDE idle (03:30–06:53; a first start at
20:00 the day before was stopped after one run because the IDE was in
use, and its data moved to `results/2026-11-03-e10-diag-aborted/`).
Checked every run (VM share, other processes, swap-ins, reclaim stalls),
then analysed with compare.py, byburst.py (--ref kairos and bpfland),
diag.py, episodes.py and overhead.py --pipe. G's load generator lag
raised the question whether the Redis p50 gap was real, so I added and
ran G-nb (`g-redis-nobatch.conf`, 07:02–07:14).
**Found:** E10: 94–96% of kairos's preemptions are for the server's
non-worker threads (7–12 µs runs). E11: delaying them (D = 1 ms, workers
exempt) removes 90% of preemptions and gives 6% more batch (5/5), as
much as bpfland, at +0.25–0.35 ms busy median; still lower than
bpfland's in 4–5/5 at every L. E12: per-CPU LC queues raise the busy
median from ~0.95 to ~1.6 ms; sync wakeups, `longest` and `prev` change
nothing measurable; sync wakeups fix the pipe test (14.9 → 1.6 µs). G:
Redis median 0.1 ms under kairos vs 0.5 under Linux and bpfland, at 6%
less batch; G-nb shows all three at 0.1–0.2 ms without batch.
**Interpretation:** Most of kairos's batch cost was preemptions for
threads that would have found a CPU within a millisecond anyway; the
latency it buys comes mainly from the shared LC queue and from
preempting for the workers. The result generalizes to Redis for
medians. Details in sections 6.22–6.25 and 7.
**Decisions:** Defaults unchanged (the new options stay off, so earlier
configs keep their meaning). `--preempt-wait-us=1000
--preempt-min-run-us=100` is the candidate for the main configuration
in later experiments; to decide with Aayush. Redis tails are not
reported (section 8).
**Problems:** Steady reclaim and compaction stalls in E10 and E11
after 5 days of uptime (section 8). G's load generator lags 1–8 ms at
the p99 at 20,000 GETs/s. E12 calibrated ~3% low (995). One overload
episode in E10 (kpw-r2, 5 s) not caused by memory.
**Next:** Decide whether pw1000r100 becomes the main kairos
configuration; restart the VM before the next long batch; the open
questions in section 9 (SCHED_IDLE's added latency, Redis's CPU under
kairos); a machine with spare CPUs for pool sizing.

### 2026-11-04 — Reviewer feedback, why ML gives no gain, plan before the VM restart
**Did:** Recorded the proposal reviewer's comment. Discussed with Aayush
why the ML predictor gives no gain and where ML could help. Checked
offline whether the E2a Azure replay has structure the predictor could
use (section 6.26). Copied this session's scratch files (analysis
helpers such as `health.py` and `wholerun.py`, run logs) from /tmp, a
tmpfs, to `~/kairos-data/scratchpad-2026-11-04/` before the restart.
**Found:** Reviewer: the evaluation plan (at least 10 interleaved runs,
medians and ranges, ablations against EEVDF, bpfland and lavd) is
rigorous; present it with statistical tests; treat the user-space
prediction daemon strictly as a stretch goal. Azure replay: little
structure at the predictor's timescale (lag-6 autocorrelation
0.04–0.14; a one-minute-earlier predictor worse than persistence in 3/3
hours).
**Interpretation:** Two reasons compound (section 7): on 3 CPUs the
right pool is all 3 CPUs, and the predictor can only raise the demand,
so it acts as a buffer; and the patterns it saw have unpredictable
onsets. A gain would need both spare CPUs and predictable onsets
(guess).
**Decisions:** (Aayush) The final evaluation comes first, then ML as a
stretch goal. The VM is restarted before the final runs (section 8,
memory after days of uptime). Proposed, to confirm when the configs are
written: 10 interleaved reps of the burst sweep and the Azure replay
with eevdf, eevdfidle, bpfland, lavd, never3, kairos and kairos
`--preempt-wait-us=1000 --preempt-min-run-us=100` in one batch, so the
main kairos configuration is chosen from the same data; tests: Wilcoxon
signed-rank paired by rep, Holm correction, bootstrap 95% CIs of median
differences. ML afterwards, if time allows: (1) the offline check in
section 9 against the EWMA plus buffer and a seasonal baseline; (2) only
if it wins there, a run with more CPUs. Another ML target is the
preemption wait D (a user-space tuner) rather than the pool size.
**Problems:** /tmp is a tmpfs, so the scratch files would have been lost
on restart.
**Next:** After the restart: write the final-evaluation configs and
`analysis/stats.py`, smoke-test them, run overnight with the machine
idle.

### 2026-11-04 — Final-evaluation configs and tests; ML offline check at 1 s
**Did:**
- Restarted the VM (no swap in use, 2.4 GB available).
- Wrote the final-evaluation configs (671776a):
  - `final-burst.conf` (E3's phases) and `final-azure.conf` (E2a's
    replay), with eevdf, eevdfidle, bpfland, lavd, lavdperf, never3,
    kairos and pw1000r100, 10 reps each.
  - `experiments/final.sh` runs both, ~6.4 h.
- Wrote `analysis/stats.py`, which has one change in byburst.py.
  Checked its Wilcoxon test against brute-force enumeration, and its
  medians against E2a's and E11's published numbers.
- Smoke-tested all 8 configs (FS).
- Then step 2 of Aayush's order: `ml/azure_predict.py` (1018388), the
  offline check of the Azure trace at 1 s (AP, section 6.27).

**Found:**
- stats.py reproduces 6.17 and 6.23 (E2a all p50: kairos 0.80, bpfland
  1.04; E11 whole-run MB/s: kairos 376, pw1000r100 399, bpfland 394).
- With 5 reps the smallest exact p is 0.0625, so nothing in E2a or E11
  is significant on its own. With 10 reps it is 0.002.
- Offline check, at equal cost:
  - The tree was lower than the best simple predictor in 0/3 of E2a's
    hours and 2/12 others, by ≤ 0.3 points.
  - The mean of the last 5 phases plus buffer was best in 10 of 15
    hours.
  - A seasonal mean foresees the :00 spikes (13–17% of onsets
    under-sized, against 31–48% for ewma + buffer), but onsets are only
    3% of samples.

**Interpretation:** At 1 s the trace is mostly counting noise around a
per-minute spike. A learned model gains nothing over simple predictors
there, so this check gives no reason for the run with more CPUs
(section 7).

**Decisions:**
- (Aayush) Order: the final evaluation first, then the 1 s offline
  check, then a run with more CPUs only if the model beats the simple
  predictors offline. It didn't, so that run is not planned.
- (Aayush) lavdperf stays in the final batch, since CLAUDE.md asks for
  both lavd modes.
- (Aayush) The offline check ran first; the batch runs tonight.
- Holm families are one metric each (7 comparisons). With 10 reps, a
  family of more than 25 tests could never reject.

**Problems:** My first matched-cost comparison stepped the extra buffer
by 0.05 CPU. Because pools are whole CPUs, that overshot the tree's cost
by up to 5x (ewma, hour 54: 0.71 vs 0.15 extra CPUs). I replaced it with
interpolation at the tree's cost.

**Next:**
- Run `experiments/final.sh` tonight, with the Mac plugged in,
  caffeinate on and the IDE idle.
- Check every run: VM share, other processes, swap-ins, reclaim stalls.
- Then `stats.py --ref kairos pw1000r100` on both parts, plus
  `byburst.py` and `bylevel.py --windows 3`.
- Choose the main kairos configuration from that data.

### 2026-11-04 — Rules for F, duplicate-pick counters, PSI; smoke run with the host slowed
**Did:**
- Discussed an outside review of the architecture with Aayush. My answer
  checked it against the code and the log. The parts already done or
  measured:
  - the deadline timer for delayed preemption;
  - "every LC task waits" (pw250);
  - accounting on the direct-dispatch path;
  - idle CPUs while work is queued (0.004 CPUs in E10).
- The parts that are new:
  - the pool formula. `ceil(demand + 1.5)` keeps the pool at 2 CPUs or
    more, and at 3 above 0.5 CPUs of demand.
  - duplicate borrower picks;
  - no batch protection (Feature B was never built);
  - `GOMAXPROCS=3` as a generality check;
  - the dependency rule as the one change likely to improve performance.
- With Aayush's OK I did items 1–3 of the plan:
  - The rules for F, in section 5.1.
  - The duplicate-pick counters and PSI (3067b18), described in
    section 4.
  - fixed3 in final-burst.conf.
- Smoke run DS (section 6.28).

**Found:**
- DS is not usable for latency or batch: the host slowed the VM (CPU per
  request +35–65% for the server and the load generator together).
- The counters work. Under that slowdown, 8.9% of kairos's preemptions
  picked a CPU already being preempted while another borrower was free
  (3.0% for pw1000r100).
- Since this morning's restart the VM runs kernel 7.0.0-42. Every earlier
  experiment ran on -38.

**Interpretation:** The duplicate share is probably inflated by the slow
host (guess), but it is well above the 1% cut-off set in the rules. The
kernel change does not affect F's internal comparisons. It does affect
comparisons with earlier experiments.

**Decisions:**
- (Aayush) The rules for F in section 5.1.
- (Aayush) A clean smoke run decides the duplicate question before F
  runs.
- Still open: the exclusion granularity (whole rep or single run), and
  the E13 success criterion.

**Problems:**
- The host slowed the VM during the smoke run, in daytime.
- An unattended kernel upgrade took effect at the restart.

**Next:**
- With the Mac idle and plugged in, rerun DS (~10 min).
- If dup_alt ≥ 1%: implement the reservation as an option, run its A/B
  (~2 h), and postpone F by a night. Otherwise run F.
- Then the dependency rule (E13).

### 2026-11-04 (evening) — DS2, `--claim-borrower` (E14), GOMAXPROCS 3
**Did:**
- Aayush ran `caffeinate` on the Mac and left it idle until 23:20.
- Reran the smoke run as DS2 (clean, section 6.29).
- Aayush decided:
  - a flagged run drops only its own pairs (`stats.py --skip-runs`,
    added);
  - if dup_alt ≥ 1%, the fix gets an A/B tonight.
- Wrote `--claim-borrower` (e11c590; section 4). Load-tested it for 8 s,
  then ran E14: 4 configs × 10 reps on the burst sweep and the Azure
  replay (3.2 h; section 6.30).
- Set the E14 adoption rule in 5.1 before E14 ran. By that rule, adopted
  the fix in F's configs (b160990).
- Used the remaining idle time for GMP3, the server at GOMAXPROCS 3, 6
  reps (6fefc35; section 6.31).

**Found:**
- DS2 is clean, and kairos's dup_alt is still 8.8%. The slow host had
  not inflated it; my guess in 6.28 was wrong.
- E14:
  - `--claim-borrower` lowers kairos's busy p50 by 2% (p = 0.008, 8/9
    reps); nothing else changes.
  - For pw1000r100 no primary metric differs at p < 0.05. But busy
    >5 ms and batch lean worse on the burst sweep (p = 0.065), and the
    10–30 ms bursts are worse at unadjusted p < 0.05.
  - The Azure replay shows no difference for either.
- GMP3: kairos < pw1000r100 < bpfland on busy p50 in 6/6 reps, and the
  reverse for batch, as at GOMAXPROCS 16. EEVDF is overloaded (busy p50
  227 ms, growing with burst length).

**Interpretation:**
- Duplicates are a real, frequent effect of deciding a preemption on one
  CPU while the victim switches later. Fixing them is worth a little
  median latency for kairos and costs nothing measurable there.
- For pw1000r100 the fix touches ~4% of decisions, so a real cost seems
  unlikely. The short-burst pattern is unexplained (6.30, 7).
- The results don't depend on GOMAXPROCS 16 for kairos. EEVDF's do.

**Decisions:**
- (Aayush) Exclusion per run, not per rep.
- (Aayush) Fix plus A/B before F.
- (by the pre-set rule) `--claim-borrower` adopted for kairos and
  pw1000r100. F's kairos, pw1000r100 and fixed3 now pass it. Aayush can
  still keep pw1000r100 without it before F runs.
- (Aayush, 2026-11-05 00:35) Stop apt-daily.timer and apt-daily-upgrade.timer
  for F's night and start F at 02:30. Stopped at 00:36 with `systemctl
  stop` (they stay enabled, so a reboot brings them back). A wrapper waits
  for 02:30 by the wall clock, runs `experiments/final.sh` and starts the
  timers again when it ends (scratchpad night.sh, copy in
  ~/kairos-data/scratchpad-2026-11-05/). Aayush didn't ask to drop the fix
  from pw1000r100, so it stays, as the rule says. Before scheduling,
  fixed3's options were loaded for 3 s: they pass the loader's check
  (`claim_borrower=1`, `lc=3-3`). man-db.timer is listed for 03:45, inside
  F's window, and was not stopped.

**Problems:**
- I chose 6 reps for GMP3 without counting Holm's 3 comparisons. n = 6
  can't reject; it would have needed 7.
- Two E14 runs had 1.6–1.9k swap-ins (about 7 MB), flagged by the rule.
- apt-daily ran at 18:14, not at 23:40 as listed, during DS2's last run
  (section 8).

**Next:**
- F (`experiments/final.sh`, ~7 h) at night with the Mac idle. It
  needs:
  - `caffeinate` for at least 7.5 h;
  - Aayush's OK to stop apt-daily.timer and apt-daily-upgrade.timer
    for the night (sudo), or a start time that avoids them.
- Aayush to confirm or change the E13 success criterion (5.1).
- Then E13, the dependency rule.

### 2026-11-05 — Final evaluation (F)
**Did:**
- Stopped apt-daily.timer and apt-daily-upgrade.timer at 00:36 (Aayush's
  OK). A wrapper started `experiments/final.sh` at 02:30 and started the
  timers again when it ended at 09:25.
- Loaded fixed3's options for 3 s first: they pass the loader's check
  with `--claim-borrower`.
- Checked all 170 runs with F's flags.
- Ran `stats.py --ref kairos pw1000r100` on both parts, then byburst.py
  and `bylevel.py --windows 3`. Outputs are in the scratchpad, copied to
  ~/kairos-data/scratchpad-2026-11-05/.
- Compared server CPU per request and batch with E14.

**Found:**
- No run flagged, so all pairs have n = 10.
- **pw1000r100 is the main configuration** by the rule in 5.1. Against
  bpfland on the burst sweep:
  - busy p50 1.41 vs 1.58 ms, lower in 10/10 (Holm 0.016);
  - batch 355 vs 360 MB/s (p = 0.92).
- On the Azure replay, pw1000r100's p50 is 12% below bpfland's (Holm
  0.014), at the same batch (p = 0.70).
- kairos has the lowest latency on both workloads (busy p50 1.10 ms;
  Azure p50 0.87), tied with fixed3 on the burst sweep (1.08, p = 0.77).
  It has 9% (7%) less batch than pw1000r100.
- fixed3 doesn't differ from kairos: kairos's pool held 3 CPUs all the
  time, on both workloads. With a 1.5-CPU buffer it can only shrink below
  0.25 CPU of LC demand (section 7). F doesn't test pool sizing.
- Two things the flags don't catch:
  - The server's CPU per request was ~10% above E14's all night, and
    batch 11–19% lower.
  - My wait process (`tail`) used 3.5–8% of a CPU in every run.

**Interpretation:**
- The delay D moves kairos along a latency–throughput trade-off. At
  1 ms it reaches bpfland's batch and keeps most of kairos's lead in
  the share over 5 ms, but only a third of its lead in median.
- The relative results match E14. The absolute levels are from a slower
  night (cause not known).

**Decisions:**
- (by the rule in 5.1) pw1000r100 is the main configuration, kairos the
  latency end.

**Problems:**
- I left a wait process running inside the VM during F. It wasn't a
  neutral load: it cost more CPU under the sched_ext schedulers than
  under EEVDF (section 8).
- I first read the wrong stats column (`lc_cpus`, the time-averaged
  pool) for the pool size; `nr_lc` is the sample.

**Next:**
- Aayush to decide whether F needs a rerun on a quieter night: no wait
  process, the Mac unused from the evening, and the apt timers stopped
  again.
- Aayush to confirm or change the E13 success criterion (5.1). Then E13.
- Why kairos's pool doesn't shrink in the burst sweep's quiet phases.
