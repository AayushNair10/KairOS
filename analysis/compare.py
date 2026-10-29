#!/usr/bin/env python3
"""Compare the configurations in one results directory, phase by phase.

For each phase and configuration: median [min-max] across repetitions of
p50, p99 and p99.9 latency (failed requests count as infinitely slow, so
an overloaded run shows inf), the median number of failed requests, batch
throughput and (scx_kairos runs) the scheduler's mean load and preemption
rate. Then, for each pair of configurations, in how many repetitions the
first had the lower p99: run.sh interleaves configurations within a
repetition, so same-numbered runs are close in time and this paired count
is robust to drift between repetitions (a sign test).

Usage: analysis/compare.py results/<date>-<name> [config ...]
(the optional list picks and orders the configurations)
"""
import os
import statistics
import sys
from itertools import combinations

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402


def med_range(xs, fmt="{:.1f}"):
    return (fmt.format(statistics.median(xs)) + " [" + fmt.format(min(xs)) + "-"
            + fmt.format(max(xs)) + "]")


def main():
    out_dir = sys.argv[1]
    groups = aggregate.runs_by_config(out_dir)
    names = sys.argv[2:] or sorted(groups)
    data = {n: [runlib.phase_rows(d) for d in groups[n]] for n in names}
    nphases = len(next(iter(data.values()))[0])

    for p in range(nphases):
        rate = next(iter(data.values()))[0][p]["target_rps"]
        print(f"== phase {p}: {rate:.0f} req/s (ms; median [min-max] over reps)")
        print(f"  {'config':8} {'p50':>18} {'p99':>22} {'p99.9':>22} {'err':>5} "
              f"{'batch MB/s':>17} {'load':>5} {'preempt/s':>9}")
        for n in names:
            rows = [r[p] for r in data[n]]
            lat = lambda k: [r[k] / 1000 for r in rows]  # noqa: E731
            sched = [r["sched"] for r in rows if r["sched"]]
            load = f"{statistics.median(s['slow_load'] for s in sched):5.2f}" if sched else "    -"
            pre = f"{statistics.median(s['preempt'] for s in sched):9.0f}" if sched else "        -"
            print(f"  {n:8} {med_range(lat('all_p50_us')):>18} {med_range(lat('all_p99_us'), '{:.0f}'):>22} "
                  f"{med_range(lat('all_p999_us'), '{:.0f}'):>22} "
                  f"{statistics.median(r['errors'] for r in rows):5.0f} "
                  f"{med_range([r['batch_mb_s'] for r in rows], '{:.0f}'):>17} {load} {pre}")
        # Paired comparison of p99 (lower is better) within each repetition.
        pairs = []
        for a, b in combinations(names, 2):
            n = min(len(data[a]), len(data[b]))
            wins = sum(data[a][i][p]["all_p99_us"] < data[b][i][p]["all_p99_us"] for i in range(n))
            pairs.append(f"{a}<{b} {wins}/{n}")
        print("  p99 lower in reps: " + ", ".join(pairs))

    print("== whole run")
    for n in names:
        sts = [runlib.system_stats(d) for d in groups[n]]
        oth = [runlib.other_cpu(d) for d in groups[n]]
        other = [100 * sum(s for _, s in o) / st["elapsed_s"] for o, st in zip(oth, sts)]
        print(f"  {n:8} ctx switches k/s {med_range([s['ctxt_per_s'] / 1000 for s in sts])}, "
              f"system time % {med_range([100 * s['system_frac'] for s in sts])}, "
              f"other processes %CPU {med_range(other)}")


if __name__ == "__main__":
    main()
