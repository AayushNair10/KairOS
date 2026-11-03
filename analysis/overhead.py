#!/usr/bin/env python3
"""Scheduler overhead (E7), from up to four result directories:

- --batch (experiments/configs/e7-batch.conf, batch jobs only): zstd MB/s
  over phase 0, context switches per second, system time and how busy
  the server's CPUs were;
- --light (e7-light.conf, light load, no batch): p50/p99 of phase 0
  (failed requests count as infinitely slow), context switches, system
  time and the server's CPU time per request;
- --pipe (experiments/e7_pipe.sh): `perf bench sched pipe` µs per round
  trip, pinned to one CPU and to two;
- --bpf (e7-bpfstats.conf, run with kernel.bpf_stats_enabled=1): per
  struct_ops program, calls per second and mean ns per call, from
  bpftool's run_cnt and run_time_ns in bpf_progs.json (counted from when
  the scheduler was loaded until just before it was unloaded), and all of
  them together as a share of one CPU. Programs that BPF timers call are
  not counted on their own (the kernel counts only program entries).

Median [min-max] over reps; then, against --ref, in how many reps each
config was better (higher MB/s, lower latency or cost).

Usage: analysis/overhead.py [--batch DIR] [--light DIR] [--pipe DIR]
       [--bpf DIR] [--ref simple] [--skip-reps N ...]
(--skip-reps leaves out whole reps of the --batch, --light and --bpf runs)
"""
import argparse
import csv
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402


def mr(xs, fmt="{:.1f}"):
    """'median [min-max]'"""
    return f"{fmt.format(statistics.median(xs))} [{fmt.format(min(xs))}-{fmt.format(max(xs))}]"


def sut_busy(run_dir, s):
    cpus = runlib.sut_cpus(run_dir)
    return sum(s["busy"][f"cpu{c}"] for c in cpus)  # in CPUs


def batch_run(d):
    p0 = runlib.load_summary(d)["phases"][0]
    s = runlib.system_stats(d)
    return {"mbs": runlib.batch_mb_per_s(d, p0["start_unix_ns"], p0["end_unix_ns"]),
            "ctxt": s["ctxt_per_s"], "sys": 100 * s["system_frac"],
            "busy": sut_busy(d, s), "vm": s["vm_share"]}


def light_run(d):
    lg = runlib.load_summary(d)
    p50, p99, _ = runlib.latency_with_failures(d, len(lg["phases"]))[0]
    s = runlib.system_stats(d)
    nreq = sum(p["requests"] for p in lg["phases"])
    return {"p50": p50 / 1000, "p99": p99 / 1000, "ctxt": s["ctxt_per_s"],
            "sys": 100 * s["system_frac"], "busy": sut_busy(d, s),
            "srv_us": 1e6 * runlib.proc_cpu(d, "grpcserver") / nreq, "vm": s["vm_share"]}


def bpf_run(d):
    """{prog name: (calls per s, ns per call)} and the total share of one CPU."""
    path = os.path.join(d, "bpf_progs.json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        progs = [p for p in json.load(f) if p.get("type") == "struct_ops" and "run_cnt" in p]
    if not progs:
        return None
    # loaded_at is in seconds; the file was written just before unloading.
    span = os.stat(path).st_mtime - min(p["loaded_at"] for p in progs)
    per = {p["name"]: (p["run_cnt"] / span, p["run_time_ns"] / p["run_cnt"] if p["run_cnt"] else 0)
           for p in progs}
    total = sum(p["run_time_ns"] for p in progs) / 1e9 / span
    return per, 100 * total


def table(title, runs, ref, cols, better):
    """cols: [(key, header, fmt)]; better[key] = +1 (higher is better) or -1."""
    print(f"== {title}")
    print("  config     " + "".join(f"{h:>22s}" for _, h, _ in cols))
    for n, rs in runs.items():
        print(f"  {n:10s} " + "".join(f"{mr([r[k] for r in rs], f):>22s}" for k, _, f in cols))
    if ref not in runs:
        return
    print(f"  reps better than {ref}: "
          + "; ".join(f"{n} " + "/".join(
              str(sum((x[k] - y[k]) * better[k] > 0 for x, y in zip(rs, runs[ref])))
              for k, _, _ in cols if k in better) + f" of {len(rs)}"
              for n, rs in runs.items() if n != ref))
    print("  (in column order: " + ", ".join(h for k, h, _ in cols if k in better) + ")")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch")
    ap.add_argument("--light")
    ap.add_argument("--pipe")
    ap.add_argument("--bpf")
    ap.add_argument("--ref", default="simple")
    ap.add_argument("--skip-reps", type=int, nargs="+", default=[])
    a = ap.parse_args()
    by_config = lambda d: sorted(aggregate.runs_by_config(d, a.skip_reps).items())  # noqa: E731

    if a.batch:
        runs = {n: [batch_run(d) for d in ds] for n, ds in by_config(a.batch)}
        table(f"batch only ({a.batch}): zstd MB/s, context switches/s, system %, "
              "server CPUs busy (CPUs), VM share", runs, a.ref,
              [("mbs", "MB/s", "{:.0f}"), ("ctxt", "ctx/s", "{:.0f}"), ("sys", "sys %", "{:.1f}"),
               ("busy", "busy CPUs", "{:.2f}"), ("vm", "VM share", "{:.2f}")],
              {"mbs": 1, "ctxt": -1, "sys": -1})
    if a.light:
        runs = {n: [light_run(d) for d in ds] for n, ds in by_config(a.light)}
        table(f"light load ({a.light}): latency ms, context switches/s, system %, "
              "server CPUs busy, server CPU µs per request, VM share", runs, a.ref,
              [("p50", "p50 ms", "{:.3f}"), ("p99", "p99 ms", "{:.2f}"), ("ctxt", "ctx/s", "{:.0f}"),
               ("sys", "sys %", "{:.1f}"), ("busy", "busy CPUs", "{:.2f}"),
               ("srv_us", "server µs/req", "{:.0f}"), ("vm", "VM share", "{:.2f}")],
              {"p50": -1, "p99": -1, "ctxt": -1, "sys": -1, "srv_us": -1})
    if a.pipe:
        rows = {}
        with open(os.path.join(a.pipe, "pipe.csv")) as f:
            for r in csv.DictReader(f):
                if r["usecs_per_op"]:
                    rows.setdefault((r["sched"], r["cpus"]), []).append(float(r["usecs_per_op"]))
        print(f"== perf bench sched pipe ({a.pipe}): µs per round trip")
        for cpus in ("0", "0,1"):
            print(f"  CPUs {cpus}: " + ", ".join(f"{s} {mr(v, '{:.2f}')}"
                                                for (s, c), v in rows.items() if c == cpus))
    if a.bpf:
        print(f"== BPF callbacks ({a.bpf}): calls/s and ns per call (median over reps); "
              "all callbacks as % of one CPU")
        for n, ds in by_config(a.bpf):
            got = [r for r in (bpf_run(d) for d in ds) if r]
            if not got:
                print(f"  {n:10s} no bpf_progs.json with run counts")
                continue
            names = sorted({k for per, _ in got for k in per})
            med = {k: (statistics.median(per[k][0] for per, _ in got if k in per),
                       statistics.median(per[k][1] for per, _ in got if k in per)) for k in names}
            print(f"  {n:10s} total {mr([t for _, t in got], '{:.2f}')}% of a CPU")
            for k in sorted(names, key=lambda k: -med[k][0] * med[k][1]):
                print(f"      {k:16s} {med[k][0]:9.0f}/s {med[k][1]:7.0f} ns")


if __name__ == "__main__":
    main()
