#!/usr/bin/env python3
"""Print a per-phase summary of one run directory written by
experiments/run.sh: request latency (from loadgen.json) next to batch
throughput (from batch_*.csv) over the same wall-clock window, then
system-wide CPU use and what else ran during the load.

Usage: analysis/summarize_run.py <run-dir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402


def main():
    run_dir = sys.argv[1]
    cols = ["phase", "target", "ok_rps", "errors", "p50_us", "p99_us", "p99.9_us",
            "max_us", "lag_p99_us", "batch_MB/s"]
    keys = ["phase", "target_rps", "ok_rps", "errors", "lat_p50_us", "lat_p99_us",
            "lat_p999_us", "lat_max_us", "send_lag_p99_us", "batch_mb_s"]
    print(" ".join(f"{c:>10}" for c in cols))
    for r in runlib.phase_rows(run_dir):
        print(" ".join(f"{r[k]:>10.0f}" for k in keys))

    lg = runlib.load_summary(run_dir)
    if "load_cpu_s" in lg:
        print(f"loadgen: {100 * lg['load_cpu_s'] / lg['load_wall_s']:.0f}% of one CPU, "
              f"GOMAXPROCS={lg['gomaxprocs']}")
    st = runlib.system_stats(run_dir)
    busy = " ".join(f"{c}={100 * b:.0f}%" for c, b in st["busy"].items())
    print(f"system: {st['ctxt_per_s'] / 1000:.1f}k ctx switches/s, "
          f"system time {100 * st['system_frac']:.1f}%, busy {busy}")
    other = runlib.other_cpu(run_dir)
    if other is not None:
        top = ", ".join(f"{c} {s / st['elapsed_s'] * 100:.1f}%" for c, s in other[:5])
        print(f"other processes (% of one CPU): {top or 'none'}")


if __name__ == "__main__":
    main()
