#!/usr/bin/env python3
"""Combine the repetitions in one results directory written by
experiments/run.sh: for each configuration (run directory name without the
-r<rep> suffix) and phase, the median and range across repetitions of the
latency percentiles, send lag and batch throughput, plus the coefficient of
variation (CV = standard deviation / mean), the run-to-run spread.

Usage: analysis/aggregate.py results/<date>-<name>
"""
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

METRICS = [("lat_p50_us", "p50 ms", 1e-3), ("lat_p99_us", "p99 ms", 1e-3),
           ("lat_p999_us", "p99.9 ms", 1e-3), ("send_lag_p99_us", "lag p99 ms", 1e-3),
           ("batch_mb_s", "batch MB/s", 1)]


def runs_by_config(out_dir):
    groups = {}
    for name in sorted(os.listdir(out_dir)):
        d = os.path.join(out_dir, name)
        m = re.fullmatch(r"(.+)-r(\d+)", name)
        if m and os.path.exists(os.path.join(d, "loadgen.json")):
            groups.setdefault(m.group(1), []).append((int(m.group(2)), d))
    return {k: [d for _, d in sorted(v)] for k, v in groups.items()}


def spread(xs):
    """'median [min-max] CV%' for a list of numbers."""
    med = statistics.median(xs)
    cv = statistics.stdev(xs) / statistics.mean(xs) if len(xs) > 1 and statistics.mean(xs) else 0
    return f"{med:8.2f} [{min(xs):.2f}-{max(xs):.2f}] {100 * cv:3.0f}%"


def main():
    out_dir = sys.argv[1]
    for config, dirs in runs_by_config(out_dir).items():
        rows = [runlib.phase_rows(d) for d in dirs]
        print(f"== {config}: {len(dirs)} reps; median [min-max] CV")
        for p in range(len(rows[0])):
            print(f"  phase {p} ({rows[0][p]['target_rps']:.0f} req/s)")
            for key, label, scale in METRICS:
                print(f"    {label:>11}: {spread([r[p][key] * scale for r in rows])}")
        sys_stats = [runlib.system_stats(d) for d in dirs]
        print(f"  {'ctx sw k/s':>13}: {spread([s['ctxt_per_s'] / 1000 for s in sys_stats])}")
        lg = [runlib.load_summary(d) for d in dirs]
        if all("load_cpu_s" in x for x in lg):
            print(f"  {'loadgen %CPU':>13}: "
                  f"{spread([100 * x['load_cpu_s'] / x['load_wall_s'] for x in lg])}")
        # Other processes: total share of one CPU, and the largest consumer.
        others = [runlib.other_cpu(d) for d in dirs]
        if all(o is not None for o in others):
            tot = [100 * sum(s for _, s in o) / st["elapsed_s"] for o, st in zip(others, sys_stats)]
            print(f"  {'other %CPU':>13}: {spread(tot)}")
            names = {}
            for o, st in zip(others, sys_stats):
                for c, s in o:
                    names.setdefault(c, []).append(100 * s / st["elapsed_s"])
            top = sorted(names.items(), key=lambda kv: -sum(kv[1]))[:4]
            print("  " + " " * 15 + "largest: " + ", ".join(
                f"{c} {sum(v) / len(dirs):.1f}%" for c, v in top))


if __name__ == "__main__":
    main()
