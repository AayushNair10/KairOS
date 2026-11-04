#!/usr/bin/env python3
"""Compare configurations by burst length, for the burst-length sweep (E3,
workloads/traces/burst_sweep.py).

The burst lengths are the durations of the phases at the highest target
rate. Phases of that duration at a lower rate are the quiet phases of the
same block; other phases (the gaps before blocks) are left out. Per length:
p50, p95, p99 and the share of requests slower than 2/5/10 ms in busy
phases (failed requests count as infinitely slow), p50 and >5 ms in quiet
phases, and batch MB/s over the block (run_stats also pools every length,
key L = "all", for stats.py). Then the same for busy requests by
time since their burst started, and for quiet requests by time since the
burst ended (a time bin pools the phases that last at least to its end).
Median over reps; then, against --ref, in how many reps each config was
better.

Usage: analysis/byburst.py results/<date>-<name> [--ref CONFIG]
       [--skip-reps N ...]
"""
import argparse
import csv
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402

SLO_MS = (2, 5, 10)
BINS_MS = (0, 2, 5, 10, 30, 100, 300, 1000, 3000, 10000)


def pct(xs, q):
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else float("nan")


def stats(lat):
    lat.sort()
    out = {"n": len(lat), "p50": pct(lat, 0.5), "p95": pct(lat, 0.95), "p99": pct(lat, 0.99)}
    for t in SLO_MS:
        out[f">{t}"] = 100 * sum(x > t for x in lat) / len(lat) if lat else float("nan")
    return out


def run_stats(run_dir):
    ph = runlib.load_summary(run_dir)["phases"]
    busy_rate = max(p["target_rps"] for p in ph)
    dur = [round(p["duration_s"] * 1000) for p in ph]
    lengths = sorted({d for d, p in zip(dur, ph) if p["target_rps"] == busy_rate})
    begin, t = [], 0.0  # phase start, ms after the schedule's start
    for d in dur:
        begin.append(t)
        t += d
    lat = {}
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            i = int(r["phase"])
            L = dur[i]
            if L not in lengths:
                continue
            v = int(r["latency_ns"]) / 1e6 if r["code"] == "0" else float("inf")
            kind = "busy" if ph[i]["target_rps"] == busy_rate else "quiet"
            lat.setdefault((kind, L), []).append(v)
            lat.setdefault((kind, "all"), []).append(v)  # every length together
            # Time since this phase started: since the burst started (busy)
            # or since it ended (quiet); the bin must end within the phase.
            off = int(r["intended_ns"]) / 1e6 - begin[i]
            for a, b in zip(BINS_MS, BINS_MS[1:]):
                if a <= off < b and b <= L:
                    lat.setdefault((kind, f"{a}-{b}"), []).append(v)
    out = {k: stats(v) for k, v in lat.items()}
    for L in lengths:  # a block = the consecutive phases of length L
        idx = [i for i, d in enumerate(dur) if d == L]
        out[("block", L)] = {"mbs": runlib.batch_mb_per_s(
            run_dir, ph[idx[0]]["start_unix_ns"], ph[idx[-1]]["end_unix_ns"])}
    return lengths, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ref")
    ap.add_argument("--skip-reps", type=int, nargs="+", default=[])
    a = ap.parse_args()
    runs = aggregate.runs_by_config(a.dir, a.skip_reps)
    data, lengths = {}, None
    for n in sorted(runs):
        data[n] = []
        for d in runs[n]:
            lengths, s = run_stats(d)
            data[n].append(s)

    def med(rs, k, c):
        xs = [r[k][c] for r in rs if k in r]
        return statistics.median(xs) if xs else float("nan")

    first = next(iter(data.values()))
    print(f"{a.dir}: median over reps (ms; >X = % of requests slower than X ms)")
    cols = ["p50", "p95", "p99"] + [f">{t}" for t in SLO_MS]
    for L in lengths:
        print(f"== bursts of {L} ms ({med(first, ('busy', L), 'n'):.0f} busy requests per run); "
              "quiet phases; batch MB/s over the block")
        print("  config     " + " ".join(f"{c:>7s}" for c in cols) + "   q.p50   q.>5    MB/s")
        for n, rs in data.items():
            print(f"  {n:10s} " + " ".join(f"{med(rs, ('busy', L), c):7.2f}" for c in cols)
                  + f" {med(rs, ('quiet', L), 'p50'):7.2f} {med(rs, ('quiet', L), '>5'):6.2f}"
                  + f" {med(rs, ('block', L), 'mbs'):7.0f}")
    bins = [f"{x}-{y}" for x, y in zip(BINS_MS, BINS_MS[1:])]
    for kind, what in (("busy", "since the burst started"), ("quiet", "since the burst ended")):
        for c in ("p50", ">5"):
            print(f"== {kind} requests by time {what} (ms): {c}")
            print("  config     " + " ".join(f"{b:>8s}" for b in bins))
            for n, rs in data.items():
                print(f"  {n:10s} " + " ".join(f"{med(rs, (kind, b), c):8.2f}" for b in bins))
    if a.ref in data:
        print(f"== reps where config is better than {a.ref} (busy p50 / busy >5 ms / block MB/s)")
        for n, rs in data.items():
            if n == a.ref:
                continue
            parts = []
            for L in lengths:
                w = [sum(x[("busy", L)][c] < y[("busy", L)][c] for x, y in zip(rs, data[a.ref]))
                     for c in ("p50", ">5")]
                w.append(sum(x[("block", L)]["mbs"] > y[("block", L)]["mbs"]
                             for x, y in zip(rs, data[a.ref])))
                parts.append(f"{L}: {'/'.join(map(str, w))}")
            print(f"  {n:10s} " + ", ".join(parts) + f" (of {len(rs)})")


if __name__ == "__main__":
    main()
