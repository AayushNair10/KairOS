# KairOS — Project Plan

The implementation plan: architecture, components, phases with dates, evaluation,
deliverables and risks. The *why* behind each decision, related work and the
detailed design (decision trees, features A and B) live in `RESEARCH_LOG.md`.

## Goal
Build a sched_ext scheduler (`scx_kairos`, written in C) that adapts wakeup
preemption to current and predicted load, so that a latency-critical service
colocated with batch work gets low tail latency when the system is quiet and
during bursts, while batch work keeps high throughput, and no task starves.

Core (must have): load-adaptive preemption + Feature A (task classification) +
Feature B (budget + max wait). Extension: user-space ML (burst prediction first,
task classification second).

---

## Architecture

```
 Ubuntu VM (4 vCPUs)
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ WORKLOADS                              MEASUREMENT                      │
 │  gRPC server (Go) / Redis ×N  ◀─────── load generator (Go, open-loop;   │
 │                                         phases or Azure trace replay)   │
 │  zstd / stress-ng (batch)               per-request latency CSV         │
 │                                                                         │
 │ USER SPACE                                                              │
 │  scx_kairos loader (C): load BPF, options, log stats                    │
 │  ML daemon (Python): every few s read stats, predict, write params      │
 │═════════════════════════════════ kernel ════════════════════════════════│
 │  scx_kairos.bpf.c (sched_ext policy, C)                                 │
 │   callbacks: select_cpu, enqueue, dispatch, runnable, running,          │
 │              stopping, quiescent, init_task                             │
 │   task storage: features + class (A)                                    │
 │   BPF timer: load EWMAs, mode state machine, budget window (B)          │
 │   maps: params (from user space), stats (to user space), ring buffer    │
 └─────────────────────────────────────────────────────────────────────────┘
```

Rule: the kernel side makes every scheduling decision on its own. User space only
loads it, reads statistics and writes parameters/predictions. Stale or missing
ML output is ignored (reactive fallback).

---

## Components

### BPF scheduler (`scheduler/scx_kairos.bpf.c`)
State:
- Global: `fast_load`, `slow_load` (EWMAs of runnable/nr_cpus ×1024), `mode`,
  latency-critical CPU time in current budget window.
- Per task (task storage): `runnable_at`, `started_at`, `avg_burst_us`,
  voluntary/involuntary stop counts (`vol_ratio`), `cpu_share`,
  `wakes_others_per_s`, `woken_by_event`, `periodicity`, `nsamples`, class.
- Per CPU: class of the currently running task (to pick a CPU to preempt).
- Params map (written by loader/ML): thresholds, smoothing shifts, budget,
  max wait, forced mode, ML burst probability + timestamp, per-task overrides.
- Stats map: preemptions, mode switches, time per mode, longest wait,
  local vs global dispatch counts.

Enqueue decision (outline):
```
class = fresh ML override, else task's own class (tree in RESEARCH_LOG §3.5)
if mode == LATENCY:
    short slice; if no idle CPU, preempt a CPU running a lower class
elif class == LATENCY_CRITICAL and budget not exhausted:
    short slice; preempt a CPU running BATCH/BACKGROUND
else:
    shared queue ordered by vtime, no preemption;
    long slice for BATCH/BACKGROUND, normal otherwise
```
Dispatch: any task waiting longer than MAX_WAIT first (Feature B), then the
shared queue. Mode tree runs in the BPF timer (RESEARCH_LOG §3.4).

Preemption mechanisms to test early: insert into the target CPU's local queue
with the preempt flag; `scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT)`; shrinking the
victim's slice (lavd's approach). Use the names valid for kernel 7.0.

### Loader (`scheduler/scx_kairos.c`)
Based on scx_simple's loader. Options: `--force-preempt`, `--never-preempt`
(static baselines from the same code), `--verbose`, parameter overrides. Writes
initial params, prints and logs stats periodically (CSV).

### Workloads (`workloads/`)
- `grpcserver/` (Go): worker pool (16–32 threads), each request burns a
  configurable amount of CPU (controls service time).
- Redis ×N (secondary, realistic).
- `loadgen/` (Go): **open-loop** (send on schedule regardless of replies, avoids
  coordinated omission), Poisson arrivals, rate schedule from a phase list or an
  Azure Functions 2021 trace (time-compressed, rate-scaled); logs send time and
  latency of every request.
- Batch: zstd on a large file (primary, cache-heavy), stress-ng (controlled).

### ML (`ml/`, Python, extension)
- Burst predictor: input = scheduler-measured load history (stats map), never
  the load generator's schedule; output = P(load > HIGH next window) + timestamp.
  Tree / boosted trees. Compare with persistence and moving average.
- Task classifier: features via ring buffer; labels from known roles; never use
  names as features; depth ≤ 5 tree; output per-task overrides.
- Daemon loop every few seconds; train/test on different patterns.

### Experiment harness and analysis
- `experiments/run.sh`: configs × repetitions, alternating order, cooldowns,
  writes `results/<date>-<name>/` + `meta.json` (via `scripts/meta.sh`).
- `analysis/plots.py`: split latency into quiet / burst / transition phases,
  percentiles, figures.

---

## Phases and dates

| Phase | Dates | Tasks | Done when |
|---|---|---|---|
| 0. Setup | Oct 24–27 | VM, sched_ext check, examples, baselines, Claude Code, GitHub | **Done** (Oct 27) |
| 1. Proposal | Oct 28–30 | Write 2-page proposal (USENIX template) | Submitted Fri Oct 30 |
| 2. Skeleton | Oct 28–Nov 3 | Copy scx_simple → scx_kairos, own build, bpf_printk in a callback | **Done** (Oct 28) |
| 3. Workloads | Oct 31–Nov 6 | gRPC server, open-loop loadgen (phases), zstd job, harness | **Done** (Oct 29) |
| 4. Load signal + static modes | Nov 3–8 | Timestamps, runnable count, wakeup rate, wait time, timer + EWMAs, forced modes, victim preemption | E1 shows the crossover. **Implemented** (Oct 29); E1 shows no latency crossover (RESEARCH_LOG 6.2); nor does E1b without batch, where preempting request threads hurts more as load rises (6.4) |
| 5. Adaptive switching | Nov 8–13 | Mode tree, hysteresis, fast exit, mode logging | Early E2; progress report Fri Nov 13. **Implemented** (Oct 30) as a latency-target switch (RESEARCH_LOG 3.4 revised, 6.8) |
| 6. Features A and B | Nov 14–21 | Features, classification tree, class-aware preemption, budget, max wait | E5, E6. **Started early** (Oct 29): batch classification + batch-only victims; E1c: best p50 at every load (RESEARCH_LOG 6.5) |
| 7. Full evaluation | Nov 18–26 | Azure replay, burst-length sweep, transitions, overhead, Redis, all baselines | E2–E7, G |
| 8. ML | Nov 24–Dec 4 | Stats export, burst predictor vs persistence, classifier if time | E8 (E9) |
| 9. Wrap-up | Dec 4–11 | Figures, analysis, video, final report | Submitted Fri Dec 11 |

---

## Evaluation (summary; details in RESEARCH_LOG §5)
Configurations: EEVDF, scx_bpfland, scx_lavd (autopilot and performance mode),
scx_kairos forced always-preempt, forced never-preempt, adaptive (± A, B, ML).

| # | Experiment | Shows |
|---|---|---|
| E1 | Steady-load sweep | Crossover between always/never preempt; calibrates LOW/HIGH |
| E2 | Bursty (synthetic + Azure) vs all baselines | Main result, per phase |
| E3 | Burst-length sweep | Burst duration vs reaction time |
| E4 | Transitions | Spikes at switches; fast exit |
| E5 | Feature A ablation | Value of classification |
| E6 | Starvation stress | B bounds longest wait |
| E7 | Overhead | Cost vs scx_simple |
| E8 | ML burst prediction | ML vs persistence vs reactive |
| E9 | ML classifier (optional) | Learned vs hand-written tree |
| G | Redis instead of gRPC | Generality |

Metrics: p50/p99/p99.9 per phase, achieved rate, batch MB/s, preemptions,
context switches, mode switches, time per mode, longest wait, runqlat,
local:global dispatch ratio.

Method: ≥ 5 reps, alternate configs, cooldowns, plugged in, Low Power Mode off,
identical VM, oversubscribe CPUs (> 1 runnable per CPU in bursts; aim 2–8).

---

## Deliverables

| Deliverable | Due | Content |
|---|---|---|
| Proposal (2 pages) | Fri Oct 30 | Motivation (fixed preemption fails at low or high load; CFS vs ULE; RUN_TO_PARITY_WAKEUP starvation); design; evaluation plan; expected outcomes and challenges |
| Progress report (3 pages) | Fri Nov 13 | Design, **screenshot of own BPF callback output**, E1, early E2, remaining work |
| Video (≤ 5 min, Zoom) | Fri Dec 11 | Idea; workload; live run showing mode switching with latency |
| Final report (5 pages) | Fri Dec 11 | Intro, background, design/impl, evaluation, discussion (why), conclusion; video link; repo with build/run instructions |

All written deliverables: USENIX LaTeX template, OSDI '27 format (two-column,
10 pt, US letter); page limits include figures and references.

---

## Risks

| Risk | Plan |
|---|---|
| Waker identification unreliable | Test in phase 4; fall back to vol_ratio, burst length, periodicity |
| Preemption mechanism misbehaves | Try preempt-flag insert and CPU kick; keep the reliable one |
| VM noise hides differences | Heavier oversubscription, more reps; cloud machine for final runs if needed |
| Adaptive barely beats best static | Still a result; E3/E4 explain where and why |
| ML doesn't beat persistence | Report and explain (bursts persist) |
| Time runs short | ML is the extension; core stands alone |
