# KairOS — Project Plan

The plan and its status: architecture, components, phases, evaluation,
deliverables and risks. The *why* behind each decision, related work, the
detailed design and every experiment live in `RESEARCH_LOG.md`.

The first version of this plan (2026-10-27) described adaptive switching
between two preemption modes. The design changed during the work. The
original plan is in git history, and RESEARCH_LOG records each change
and why.

## Status (2026-11-06)
- **Evaluation done.** Every planned experiment has run or been replaced
  (table below). The final evaluation F and the follow-ups E13, E15 and
  E5 are analysed under rules set before their data existed (RESEARCH_LOG
  5.1).
- **Main configuration:** kairos-pw1000r100 with the starvation guard.
  That is the soft CPU split with delayed borrower preemption (1 ms), one
  preemption per borrower and a 2 s maximum wait. Run it with
  `scx_kairos --main-config`.
- **Left:**
  - figures;
  - three short write-ups in the log (strict priority and kpw-r2, the
    Redis per-request check, transitions from F's data);
  - the progress report, final report and video.

---

## Goal
Build a sched_ext scheduler (`scx_kairos`, written in C) that adapts wakeup
preemption to current and predicted load, so that a latency-critical service
colocated with batch work gets low tail latency when the system is quiet and
during bursts, while batch work keeps high throughput, and no task starves.

Planned core: load-adaptive preemption + Feature A (task classification) +
Feature B (budget + max wait). Extension: user-space ML.

As built, load adaptation is a soft CPU split:
- an LC pool sized to measured LC demand;
- batch borrows idle LC CPUs;
- LC wakeups preempt borrowers, at once or after a short wait.

Feature A is classification by scheduling policy, and Feature B is the
maximum wait. The ML extension was built, measured and closed.

---

## Architecture (as built)

```
 Ubuntu VM (4 vCPUs)
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ WORKLOADS (CPUs 0-2)                   MEASUREMENT (CPU 3)              │
 │  gRPC server (Go) / Redis ×N  ◀─────── load generator (Go, open-loop;   │
 │  zstd (batch, SCHED_BATCH)              phases or Azure trace replay)   │
 │                                         per-request latency CSV         │
 │ USER SPACE                                                              │
 │  scx_kairos loader (C): options, load BPF, per-second stats (CSV);      │
 │  optional: evaluates an LC-demand tree (--ml-model) and writes it in    │
 │═════════════════════════════════ kernel ════════════════════════════════│
 │  scx_kairos.bpf.c (sched_ext policy, C)                                 │
 │   callbacks: select_cpu, enqueue, dispatch, runnable, running,          │
 │              stopping, quiescent, enable, init_task, init, exit         │
 │   queues: LC_DSQ and BATCH_DSQ (vtime-ordered), per-CPU local queues    │
 │   task storage: run times, wait start, class                            │
 │   BPF timers: load signal and LC pool size (every 1 ms);                │
 │               delayed borrower preemption (--preempt-wait-us)           │
 │   maps: stats (per-CPU counters), diag (per task group)                 │
 └─────────────────────────────────────────────────────────────────────────┘
```

Rule: the kernel side makes every scheduling decision on its own. User
space only loads it, reads statistics and, with `--ml-model`, writes a
prediction. Stale predictions are ignored.

---

## Components (as built)

### BPF scheduler (`scheduler/scx_kairos.bpf.c`)
The top comment describes every part. In short:
- **Split** (`--split`):
  - The split CPUs are LC CPUs (the first `nr_lc`) and batch CPUs. Each
    pool serves its own queue first and lends idle CPUs to the other.
  - The timer sizes the LC pool as ceil(LC demand + buffer), and shrinks
    it only with hysteresis.
- **A woken LC task**, in order:
  1. takes an idle CPU, LC CPUs first;
  2. otherwise preempts a batch task borrowing an LC CPU;
  3. otherwise queues.

  With `--preempt-wait-us`, a short LC task first waits up to that long
  for an LC CPU.
- **Classification** (Feature A): by scheduling policy (`--class=policy`)
  or by CPU used since the task last slept (`--class=run`). E5 found the
  policy rule much better. The multi-feature tree of RESEARCH_LOG 3.5
  was not built.
- **Starvation guard** (Feature B, `--max-wait-ms`). A task of the other
  pool that has waited past the limit runs before a CPU's own queue. The
  CPU-time budget was not built.
- **The first design is kept for baselines and ablations:** two modes
  (BATCH, LATENCY), forced or switched against a latency target.

### Loader (`scheduler/scx_kairos.c`)
- Based on scx_simple's loader.
- Options set the read-only tunables; `--help` groups them, and
  `--main-config` sets the main configuration.
- It prints and logs per-second stats, and optionally per-task-group
  diagnostics (`--diag`).

### Workloads (`workloads/`)
- `grpcserver/` (Go): a worker pool (16 threads); each request burns a
  set amount of CPU (500 µs in the main experiments).
- Redis ×N (generality check G).
- `loadgen/` (Go):
  - open-loop, Poisson arrivals;
  - phases from a list (`traces/burst_sweep.py`) or an Azure Functions
    2021 replay (`traces/azure_phases.py`);
  - logs every request.
- Batch: zstd loops over a 64 MB tar of the kernel source
  (`batch/zstd_loop.sh`).

### ML (`ml/`, extension, closed)
- `train_burst.py` trains a tree that predicts LC demand; the loader
  evaluates it (`--ml-model`).
- `azure_predict.py`: offline predictors on the Azure trace.
- Result: equivalent to a bigger buffer on 3 CPUs (RESEARCH_LOG 6.13,
  6.26, 6.27).
- The task classifier (E9) and the daemon were not built.

### Experiment harness and analysis
- `experiments/run.sh`:
  - runs configs × reps, with the order reversed every other rep, and
    cooldowns;
  - checks host speed when a part starts;
  - writes `results/<date>-<name>/` and `meta.json`.
- `analysis/`:
  - per-run summaries and per-phase comparisons;
  - burst-length and load-level breakdowns;
  - statistical tests (`stats.py`: Wilcoxon signed-rank paired by rep,
    Holm, bootstrap CIs);
  - overhead and diagnostics.
- Figures: not yet.

---

## Phases and dates

| Phase | Dates | Tasks | Status |
|---|---|---|---|
| 0. Setup | Oct 24–27 | VM, sched_ext check, examples, baselines, Claude Code, GitHub | **Done** (Oct 27) |
| 1. Proposal | Oct 28–30 | Write 2-page proposal (USENIX template) | Due Fri Oct 30 |
| 2. Skeleton | Oct 28–Nov 3 | Copy scx_simple → scx_kairos, own build, bpf_printk in a callback | **Done** (Oct 28) |
| 3. Workloads | Oct 31–Nov 6 | gRPC server, open-loop loadgen (phases), zstd job, harness | **Done** (Oct 29) |
| 4. Load signal + static modes | Nov 3–8 | Load signal, timer + EWMAs, forced modes, victim preemption | **Done** (Oct 29). E1 and E1b show no latency crossover (6.2, 6.4) |
| 5. Adaptive switching | Nov 8–13 | Mode switching | **Done** (Oct 30), then **replaced** by the soft CPU split (E2, E2s: 6.9, 6.12) |
| 6. Features A and B | Nov 14–21 | Classification, max wait | **Done.** Classification by run time (Oct 29) and by policy. Max wait (`--max-wait-ms`, Nov 5; GT, guard check). Budget and tree not built |
| 7. Full evaluation | Nov 18–26 | Azure replay, burst-length sweep, overhead, Redis, all baselines | **Done** (Nov 1–6): E2a, E3, E7, E10–E15, G, F, E5 |
| 8. ML | Nov 24–Dec 4 | Burst predictor vs simple ones; classifier if time | **Closed** (Nov 5), negative results: predictor ≈ bigger buffer; classifier not built |
| 9. Wrap-up | Dec 4–11 | Figures, analysis, video, final report | **In progress:** README, `--main-config` done; figures next |

---

## Evaluation (as run; details in RESEARCH_LOG sections 5 and 6)
Configurations:
- EEVDF, and EEVDF with batch as SCHED_IDLE;
- scx_bpfland;
- scx_lavd, in autopilot and performance mode;
- kairos forced never-preempt;
- kairos (borrowers preempted at once);
- kairos-pw1000r100 (1 ms wait);
- ablations.

| # | Experiment | Status | Result | Section |
|---|---|---|---|---|
| E1 | Steady-load sweep | Done | No latency crossover between always and never preempt | 6.2–6.5 |
| E2 | Bursty load vs baselines | Done | Mode switching gained little; the soft CPU split replaced it | 6.9–6.17 |
| E3 | Burst-length sweep | Done | kairos busy p50 lowest at every burst length, for 6–20% less batch | 6.19, 6.21 |
| E4 | Transitions | Not run on its own | To be written up from F's burst-sweep data | — |
| E5 | Classification ablation | Done (Nov 6) | Run-time rule: busy p50 ×2.2 for +7% batch; labels kept | 6.38 |
| E6 | Starvation stress | Done as GT | Without the guard a batch task ran 0 ms in 10 s; with it, waits stay ≈ 2 s | 6.33 |
| E7 | Overhead | Done | Batch-only within ±3% of scx_simple; callbacks 4.1% of a CPU | 6.20 |
| E8 | ML burst prediction | Done, closed | ≈ a bigger buffer; offline, the tree didn't beat simple predictors on the Azure trace | 6.13, 6.26, 6.27 |
| E9 | ML classifier | Not done | Closed with the ML work | — |
| E10 | Which wakeups preempt | Done | Mostly the server's short non-worker threads | 6.22 |
| E11 | Delayed borrower preemption | Done | 1 ms wait: 90% fewer preemptions, batch as bpfland's, latency between kairos and bpfland | 6.23 |
| E12 | Ablations | Done | The shared LC queue matters; sync, borrower choice don't | 6.24 |
| E13 | Dependency rule for the wait | Done (Nov 6) | No window passes; not adopted | 6.35 |
| E14 | One preemption per borrower | Done | Adopted (`--claim-borrower`) | 6.30 |
| E15 | Pool sizing | Done (Nov 6) | Doesn't pay on 3 CPUs | 6.37 |
| G | Redis | Done | Redis p50 0.1 ms under kairos vs 0.5 under Linux and bpfland | 6.25 |
| F | Final evaluation | Done (Nov 5) | kairos-pw1000r100 beats bpfland on busy p50 at equal batch; kairos lowest latency | 6.32 |

Method:
- 10 interleaved reps in most experiments (3–5 in a few, such as E3),
  with cooldowns;
- decision rules set before the data (RESEARCH_LOG 5.1);
- run checks: VM share, other processes, swapping, reclaim, host speed.

---

## Deliverables

| Deliverable | Due | Content | Status |
|---|---|---|---|
| Proposal (2 pages) | Fri Oct 30 | Motivation; design; evaluation plan; expected outcomes and challenges | |
| Progress report (3 pages) | Fri Nov 13 | Design, **screenshot of own BPF callback output**, E1, early E2, remaining work | Not started; needs the screenshot |
| Video (≤ 5 min, Zoom) | Fri Dec 11 | Idea; workload; a live run showing latency | Not started |
| Final report (5 pages) | Fri Dec 11 | Intro, background, design/impl, evaluation, discussion (why), conclusion; video link; repo with build/run instructions | Not started; README with build/run instructions done |

All written deliverables: USENIX LaTeX template, OSDI '27 format (two-column,
10 pt, US letter); page limits include figures and references.

---

## Risks (how they turned out)

| Risk | Plan | Outcome |
|---|---|---|
| Waker identification unreliable | Fall back to simpler features | The policy rule was used. E13's waker rule fired for most wakeups and didn't help |
| Preemption mechanism misbehaves | Try preempt-flag insert and CPU kick | Preempt-flag insert into the borrower's local queue works. Duplicate picks were fixed by `--claim-borrower` |
| VM noise hides differences | More reps, run checks | 10 reps, run checks and paired tests. Host slowdowns still happened (RESEARCH_LOG section 8) |
| Adaptive barely beats best static | Still a result | Mode switching didn't; the soft CPU split did |
| ML doesn't beat persistence | Report and explain | It didn't; reported as negative results |
| Time runs short | ML is the extension | Core done; ML closed |
