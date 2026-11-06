# KairOS

`scx_kairos` is a Linux CPU scheduler, written in C with
[sched_ext](https://docs.kernel.org/scheduler/sched-ext.html) (eBPF). It
targets a latency-critical (LC) service that shares CPUs with batch work.
The aim is low request latency for the service, while batch work keeps
most of its throughput.

## How it works

- **Soft CPU split** (`--split`). The CPUs it manages form two pools:
  - **LC CPUs**, a pool sized to the service's measured CPU demand plus a
    buffer;
  - **batch CPUs**, the rest.

  Each pool serves its own queue first and lends idle CPUs to the other.
  A batch task on an LC CPU (a *borrower*) gets a short slice.
- **Preempting borrowers.** A woken LC task that finds no idle CPU
  preempts a borrower. With `--preempt-wait-us`, a short LC task first
  waits up to that long for an LC CPU to free up. That trades a little
  latency for batch throughput.
- **One preemption per borrower** (`--claim-borrower`). A second LC
  wakeup can't pick a borrower that is already being preempted.
- **Starvation guard** (`--max-wait-ms`). When a CPU is about to run
  its own pool's queue, a task of the other pool that has waited past the
  limit runs first.
- **Classification by scheduling policy** (`--class=policy`). Batch work
  is whatever runs as `SCHED_BATCH` or `SCHED_IDLE`, e.g. under
  `chrt --batch 0`.

The policy is `scheduler/scx_kairos.bpf.c`; its top comment describes
every part. The loader is `scheduler/scx_kairos.c`.

## Results

These come from a 4-vCPU VM (Apple M2 host, Linux 7.0):
- a Go gRPC server and 2 zstd jobs ran on CPUs 0–2;
- an open-loop load generator ran on CPU 3;
- 10 interleaved reps per configuration;
- tests: Wilcoxon signed-rank paired by rep, with Holm correction.

`docs/RESEARCH_LOG.md` (section 6.32) has the full results and the
rules set before the runs.

**Burst sweep** (busy 3,500 req/s and quiet 500 req/s phases, 10 ms to
10 s long). Medians:

| scheduler | busy p50 (ms) | busy requests over 5 ms | batch (MB/s) |
|---|---|---|---|
| kairos (no wait) | 1.10 | 7.6% | 324 |
| **kairos-pw1000r100** (1 ms wait) | 1.41 | 9.2% | 355 |
| scx_bpfland | 1.58 | 16.2% | 360 |
| EEVDF, batch as SCHED_IDLE | 2.37 | 24.0% | 341 |
| EEVDF (Linux default) | 4.11 | 43.3% | 365 |
| scx_lavd | 8.62 | 73.4% | 386 |

- **kairos-pw1000r100 is the main configuration.**
  - Against scx_bpfland it has lower busy p50 in 10/10 reps (Holm
    p = 0.016), and no difference in batch (p = 0.92).
  - The starvation guard was added to it later (`--main-config`). It
    never fired in the burst sweep and made no difference at p < 0.05
    (section 6.36).
- **Without the wait** (the first row), kairos has the lowest latency
  in the table, for about 9% less batch.
- **On a replay of Azure Functions traffic** the top three keep their
  order: p50 0.87 ms for kairos, 1.02 for kairos-pw1000r100 and 1.14 for
  bpfland. EEVDF is last, at 1.66 ms.

Ideas that didn't pay on this machine are in the log too (section 6):
- a dependency rule for the wait;
- sizing the LC pool more tightly;
- classifying tasks by run time instead of policy;
- an ML predictor of LC demand.

## Requirements

- **A kernel with sched_ext** (`CONFIG_SCHED_CLASS_EXT=y`). Developed
  and tested only on Linux 7.0 (Ubuntu 26.04, arm64).
  - kfunc names change between kernel versions. The code follows the
    `tools/sched_ext` examples of 7.0, so older kernels may need changes.
  - Check: `cat /sys/kernel/sched_ext/state`.
- **The kernel source for the running kernel,** e.g. in `~/linux-src`.
  The build uses its `tools/sched_ext` headers, libbpf and bpftool.
- **Packages (Ubuntu):** `clang llvm llvm-dev libelf-dev zlib1g-dev
  libssl-dev libcap-dev binutils-dev pkg-config make`.
- **For the workloads:** Go ≥ 1.26 (`golang-go`), `zstd`, and for the
  Redis variant `redis-server`.
- **Python 3.** The analysis scripts use only the standard library.
- **Optional, for the baselines:** `scx_bpfland` and `scx_lavd` from
  [sched-ext/scx](https://github.com/sched-ext/scx), built with cargo
  (`experiments/run.sh` looks in `~/scx/target/release`).

## Build

```sh
# 1. The kernel's sched_ext examples, once. This produces libbpf, bpftool
#    and vmlinux.h for the running kernel. Repeat it after a kernel update.
make -C ~/linux-src/tools/sched_ext

# 2. The scheduler (set KSRC=... if the kernel source is elsewhere).
make -C scheduler

# 3. The workloads (gRPC server and load generator, into workloads/bin/).
make -C workloads

# 4. The batch jobs' input: 64 MB of a tar of the kernel source, once.
./workloads/batch/make_input.sh      # -> ~/kairos-data/zstd-input-64M.tar
```

## Run the scheduler

```sh
sudo ./scheduler/scx_kairos --main-config
```

- **`--main-config`** is the configuration the evaluation chose. It
  expands to:

  ```
  --split --class=policy --lc-buffer-pct=150 --claim-borrower
  --preempt-wait-us=1000 --preempt-min-run-us=100 --max-wait-ms=2000
  ```

  Options after it override it.
- **Batch work must be labelled.** Start it with `chrt --batch 0 CMD` or
  `chrt --idle 0 CMD`. Everything else is treated as latency-critical.
- **`--split-cpus LIST`** limits the scheduler to some CPUs. The
  experiments use `--split-cpus=0-2` because the load generator has
  CPU 3.
- **Output.** The loader prints its counters once a second.
  `--stats FILE` also writes them as CSV.
- **Unloading.** Ctrl-C unloads the scheduler and the kernel goes back
  to its default. If the scheduler misbehaves, the sched_ext watchdog
  unloads it; `dmesg` shows why.
- **Is it running?** `cat /sys/kernel/sched_ext/state` and
  `cat /sys/kernel/sched_ext/root/ops`.
- **All options:** `./scheduler/scx_kairos --help`. The options for
  experiments and ablations only are marked there.

## Run the experiments

Each experiment is a config file in `experiments/configs/`.
`experiments/run.sh` runs each config file:
- every scheduler in it, once per rep, with the order reversed every
  other rep;
- before each run, it starts the server, the batch jobs and the load
  generator;
- each run's raw data goes to `results/<date>-<name>/<scheduler>-r<rep>/`,
  with a `meta.json` (git commit, kernel, options).

It loads schedulers with `sudo`. Keep the machine otherwise idle, and
keep a laptop host awake and plugged in (e.g. `caffeinate` on macOS).

```sh
./experiments/run.sh experiments/configs/smoke.conf     # a quick check
./experiments/final.sh                                  # the final evaluation, ~7 h
```

`results/` isn't in git (about 18 GB here). The numbers are in
`docs/RESEARCH_LOG.md`.

## Analyse

```sh
./analysis/summarize_run.py results/<date>-<name>/<run>          # one run
./analysis/stats.py results/<date>-<name> --ref kairos            # tests against a reference
./analysis/byburst.py results/<date>-<name> --ref kairos          # burst sweep, per burst length
./analysis/bylevel.py results/<date>-<name> --ref kairos          # Azure replay, per load level
```

## Layout

```
scheduler/   scx_kairos.bpf.c (policy), scx_kairos.c (loader), scx_kairos.h, Makefile
workloads/   grpcserver/ and loadgen/ (Go), batch/ (zstd), traces/ (phase generators)
experiments/ run.sh (harness), configs/, scripts that run several configs in a row
analysis/    per-run summaries, comparisons, statistical tests
ml/          the LC-demand predictor (an extension; no gain measured)
scripts/     helpers (run metadata, sampling, cache eviction, log entries)
docs/        PROJECT_PLAN.md (plan and status), RESEARCH_LOG.md (decisions, every experiment, results)
```

## License

The scheduler is GPL-2.0, like the sched_ext examples it is based on
(scx_simple, Copyright (c) 2022 Meta Platforms, Inc. and affiliates).
