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

SWAP_IN_WARN = 1000  # pages (4 KiB) swapped in during the load


def main():
    run_dir = sys.argv[1]
    cols = ["phase", "target", "ok_rps", "errors", "p50_us", "p99_us", "p99.9_us",
            "max_us", "lag_p99_us", "batch_MB/s"]
    keys = ["phase", "target_rps", "ok_rps", "errors", "lat_p50_us", "lat_p99_us",
            "lat_p999_us", "lat_max_us", "send_lag_p99_us", "batch_mb_s"]
    print(" ".join(f"{c:>10}" for c in cols))
    rows = runlib.phase_rows(run_dir)
    for r in rows:
        print(" ".join(f"{r[k]:>10.0f}" for k in keys))
    if any(r["sched"] for r in rows):
        print("scheduler (per-second means): phase, runnable, slow_load, wakeups/s, "
              "preempt/s, wait_avg_us, wait_max_us")
        for r in rows:
            s = r["sched"]
            if s:
                print(f"  {r['phase']:>3} {s['nr_runnable']:6.1f} {s['slow_load']:6.2f} "
                      f"{s['wake_rate']:9.0f} {s['preempt']:8.0f} {s['wait_avg_us']:8.0f} "
                      f"{s['wait_max_us']:8.0f}")

    lg = runlib.load_summary(run_dir)
    if "load_cpu_s" in lg:
        print(f"loadgen: {100 * lg['load_cpu_s'] / lg['load_wall_s']:.0f}% of one CPU, "
              f"GOMAXPROCS={lg['gomaxprocs']}")
    st = runlib.system_stats(run_dir)
    busy = " ".join(f"{c}={100 * b:.0f}%" for c, b in st["busy"].items())
    print(f"system: {st['ctxt_per_s'] / 1000:.1f}k ctx switches/s, "
          f"system time {100 * st['system_frac']:.1f}%, busy {busy}")
    warn = "  WARNING: the host held the VM back" if st["vm_share"] < 0.95 else ""
    print(f"VM time share: {st['vm_share']:.2f} of the wall time{warn}")
    mem = runlib.memory_stats(run_dir)
    if mem is not None:
        # The threshold is a first guess: ~3,000 swap-ins came with slow
        # seconds in the E3 diagnostic run; refine it once clean runs exist.
        warn = "  WARNING: the VM was swapping" if mem["swap_in"] > SWAP_IN_WARN else ""
        print(f"memory: {mem['swap_in']} pages swapped in, {mem['swap_out']} out, "
              f"{mem['major_faults']} major faults, {mem['reclaim_stalls']} direct-reclaim "
              f"stalls, {mem['compact_stalls']} compaction stalls{warn}")
    other = runlib.other_cpu(run_dir)
    if other is not None:
        top = ", ".join(f"{c} {s / st['elapsed_s'] * 100:.1f}%" for c, s in other[:5])
        print(f"other processes (% of one CPU): {top or 'none'}")


if __name__ == "__main__":
    main()
