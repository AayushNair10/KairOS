#!/usr/bin/env python3
"""Print a per-phase summary of one run directory written by
experiments/run.sh: request latency (from loadgen.json) next to batch
throughput (from batch_*.csv) over the same wall-clock window.

Usage: analysis/summarize_run.py <run-dir>
"""
import csv
import glob
import json
import os
import sys


def batch_mb_per_s(run_dir, start_ns, end_ns):
    """Batch throughput in MB/s of zstd input over [start_ns, end_ns).

    Each completed pass counts in proportion to how much of it overlaps the
    window, assuming zstd works through its input at a steady rate.
    """
    done_bytes = 0.0
    for path in glob.glob(os.path.join(run_dir, "batch_*.csv")):
        with open(path) as f:
            for row in csv.DictReader(f):
                s = int(row["start_unix_us"]) * 1000
                e = int(row["end_unix_us"]) * 1000
                overlap = min(e, end_ns) - max(s, start_ns)
                if overlap > 0:
                    done_bytes += int(row["bytes"]) * overlap / (e - s)
    return done_bytes / 1e6 / ((end_ns - start_ns) / 1e9)


def main():
    run_dir = sys.argv[1]
    with open(os.path.join(run_dir, "loadgen.json")) as f:
        lg = json.load(f)
    cols = ["phase", "target", "ok_rps", "errors", "p50_us", "p99_us", "p99.9_us",
            "max_us", "lag_p99_us", "batch_MB/s"]
    print(" ".join(f"{c:>10}" for c in cols))
    for p in lg["phases"]:
        mbps = batch_mb_per_s(run_dir, p["start_unix_ns"], p["end_unix_ns"])
        vals = [p["phase"], p["target_rps"], p["ok_rps"], p["errors"], p["lat_p50_us"],
                p["lat_p99_us"], p["lat_p999_us"], p["lat_max_us"], p["send_lag_p99_us"], mbps]
        print(" ".join(f"{v:>10.0f}" for v in vals))


if __name__ == "__main__":
    main()
