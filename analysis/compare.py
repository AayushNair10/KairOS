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

Usage: analysis/compare.py results/<date>-<name> [--skip-reps N,...] [config ...]
(the optional list picks and orders the configurations; --skip-reps leaves
out whole reps, e.g. ones disturbed by the host)
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
    out_dir, args = sys.argv[1], sys.argv[2:]
    skip = ()
    if args[:1] == ["--skip-reps"]:
        skip, args = tuple(int(x) for x in args[1].split(",")), args[2:]
    groups = aggregate.runs_by_config(out_dir, skip)
    names = args or sorted(groups)
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
            # immediate preemptions plus the delayed-preemption timer's
            pre = (f"{statistics.median(s['preempt'] + s.get('late_preempt', 0) for s in sched):9.0f}"
                   if sched else "        -")
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
        # the service: grpcserver, or the redis-server instances
        server = [100 * (runlib.proc_cpu(d, "grpcserver") + runlib.proc_cpu(d, "redis-server"))
                  / st["elapsed_s"] for d, st in zip(groups[n], sts)]
        # Idle time summed over the server's CPUs, in % of one CPU.
        idle = [sum(100 * (1 - st["busy"][f"cpu{c}"]) for c in runlib.sut_cpus(d))
                for d, st in zip(groups[n], sts)]
        print(f"  {n:8} ctx switches k/s {med_range([s['ctxt_per_s'] / 1000 for s in sts])}, "
              f"system time % {med_range([100 * s['system_frac'] for s in sts])}, "
              f"other processes %CPU {med_range(other)}, "
              f"server %CPU {med_range(server)}, idle on server CPUs %CPU {med_range(idle)}, "
              f"VM time share {med_range([s['vm_share'] for s in sts], '{:.2f}')}")
        mem = [runlib.memory_stats(d) for d in groups[n]]
        if all(m is not None for m in mem):
            print(f"  {'':8} pages swapped in {med_range([m['swap_in'] for m in mem], '{:.0f}')}, "
                  f"direct-reclaim stalls {med_range([m['reclaim_stalls'] for m in mem], '{:.0f}')}")


if __name__ == "__main__":
    main()
