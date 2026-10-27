# CLAUDE.md — adaptive-sched

Instructions for Claude when working in this repository.

## What this project is
CSE 506 (Fall 2026) individual course project by Aayush Nair: a Linux scheduler
built with **sched_ext** (eBPF) that adapts **wakeup preemption** to current and
predicted load, for a latency-critical service colocated with batch work.
Full design, decisions and related work: `docs/RESEARCH_LOG.md`. Read it before
starting any non-trivial task.

AI assistance is allowed by the course, but **I must understand every line of
code and verify every result**. So:
- Explain non-obvious code you write (BPF verifier constraints, fixed-point math,
  concurrency, kfunc semantics) in short comments or in your reply.
- Prefer small, reviewable changes over large rewrites.
- Never invent results. If something wasn't measured, say so.

## Language
The scheduler (BPF policy and user-space loader) is written in **C**, based on
the kernel's `tools/sched_ext` examples (scx_simple). Do not introduce Rust.
Rust is used only to build the baseline schedulers scx_bpfland and scx_lavd.
Helper tooling (load generator, ML, analysis) may use Go or Python.

## Environment
- Ubuntu 26.04 LTS ARM64 in a Lima VM (`sched`) on a MacBook Air M2. 4 vCPUs,
  4 GB RAM + 4 GB swap. The Mac home folder is mounted read-only; the repo lives
  in the VM's home (`~/adaptive-sched`).
- Kernel source for the examples: `~/linux-src` (tag matching `uname -r`);
  build examples with `make -C ~/linux-src/tools/sched_ext` (needs libssl-dev,
  libcap-dev, binutils-dev, llvm-dev for bpftool). Binaries land in
  `~/linux-src/tools/sched_ext/build/bin/`.
- Baselines: scx_bpfland and scx_lavd 1.1.4, built with cargo in
  `~/scx/target/release/` (no Ubuntu package exists). Run lavd both in its
  default autopilot mode and its performance mode (core compaction otherwise
  confines light load to CPU 0).
- Kernel must be ≥ 6.12 with `CONFIG_SCHED_CLASS_EXT=y`. Check:
  `uname -r`, `cat /sys/kernel/sched_ext/state`.
- sched_ext kfunc names differ between kernel versions (e.g. `scx_bpf_dispatch`
  became `scx_bpf_dsq_insert` in 6.13). **Always match the names used by the
  example schedulers built for the installed kernel**; check
  `tools/sched_ext/` or the scx repo's headers rather than relying on memory.
- Loading a scheduler needs root (`sudo`). Ask before running anything with sudo
  other than loading/unloading our scheduler or reading tracing output.

## Repository layout
```
scheduler/      adaptive.bpf.c (policy), adaptive.c (loader/stats), Makefile
ml/             collect.py, train_burst.py, train_classifier.py, daemon.py
workloads/      grpcserver/ (Go), loadgen/ (Go, open-loop), traces/
experiments/    run.sh (harness), configs/
analysis/       plots.py
results/        <date>-<name>/ raw data + meta.json (never edit raw data)
docs/           RESEARCH_LOG.md, proposal/, report/
scripts/        helpers (log_entry.sh, meta.sh)
```

## Commands
(Fill in as they become real.)
- Build scheduler: `make -C scheduler`
- Load: `sudo ./scheduler/adaptive [--force-preempt|--never-preempt] [--verbose]`
  (Ctrl-C unloads; the kernel falls back to EEVDF)
- Trace output: `sudo cat /sys/kernel/debug/tracing/trace_pipe`
- Is it running: `cat /sys/kernel/sched_ext/state` and `cat /sys/kernel/sched_ext/root/ops`
- Run an experiment: `./experiments/run.sh experiments/configs/<name>.conf`
- New log entry skeleton: `./scripts/log_entry.sh "<title>"`

## Coding rules
- BPF: keep loops bounded (`bpf_for`), null-check every map/task-storage lookup,
  integer/fixed-point arithmetic only (no floats), keep callbacks short.
- All tunables (thresholds, smoothing shifts, budget, max wait) live in a params
  map or are command-line options, never hard-coded constants scattered around.
- The scheduler must support forced static modes (always/never preempt) from the
  same code, so baselines differ only in policy.
- Count everything you might want to plot (preemptions, mode switches, time per
  mode, longest wait) in a stats map; the loader logs it.
- Every experiment run writes `results/<date>-<name>/meta.json` with git commit,
  kernel version, VM config, scheduler options and workload parameters.
- Commit early and often with descriptive messages.

## Safety
- A misbehaving scheduler is ejected by the sched_ext watchdog; this is expected
  during development. Check `dmesg` for the exit reason.
- Don't modify or install kernels, change bootloader settings, or delete files
  outside this repository without asking.

## Research log: required after every task
At the end of **every** task or working session, append an entry to section 10
of `docs/RESEARCH_LOG.md` using its template (Did / Found / Interpretation /
Decisions / Problems / Next). Also:
- Add any experiment run to section 5 (experiment log table) with its data path
  and commit.
- Put new measured results in section 6 and explanations in section 7.
- Record API names, verifier issues and mechanism choices in section 4.
- Record changed decisions as new entries; don't rewrite old ones.
- Keep entries factual and short; mark guesses as guesses.
