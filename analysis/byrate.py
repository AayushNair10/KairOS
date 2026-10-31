#!/usr/bin/env python3
"""Compare configurations by load level rather than phase by phase.

For patterns with many phases (ml/patterns.py): requests in busy phases
(target rate >= --busy, default 2000 req/s) and in quiet phases are pooled
per run; "onset" is the busy requests sent in the first --onset-ms of a
busy phase. Per run: p50 and p99 (failed requests count as infinitely
slow), batch MB/s over all busy / quiet time, and for --split runs the mean
LC CPUs and how often an ML prediction raised the demand; the share of
requests slower than 2/5/10/50 ms and busy p90/p95. Then the median
over reps and, against --ref, in how many reps each config was better.

Usage: analysis/byrate.py results/<date>-<name> [--ref CONFIG] [--skip-reps N ...]
"""
import argparse
import csv
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402


SLO_MS = (2, 5, 10, 50)  # thresholds for "% of requests slower than"


def pct(xs, q):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def run_stats(run_dir, busy_rps, onset_ms):
    lg = runlib.load_summary(run_dir)
    st = lg["start_unix_ns"]
    ph = {p["phase"]: p for p in lg["phases"]}
    lat = {"busy": [], "quiet": [], "onset": []}
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            p = ph[int(r["phase"])]
            v = int(r["latency_ns"]) / 1e6 if r["code"] == "0" else float("inf")
            if p["target_rps"] >= busy_rps:
                lat["busy"].append(v)
                if int(r["intended_ns"]) - (p["start_unix_ns"] - st) < onset_ms * 1e6:
                    lat["onset"].append(v)
            else:
                lat["quiet"].append(v)
    out = {}
    for k, v in lat.items():
        out[k + "_p50"], out[k + "_p99"] = pct(v, 0.5), pct(v, 0.99)
    for k in ("quiet", "busy"):
        for t in SLO_MS:
            out[f"{k}>{t}"] = 100 * sum(x > t for x in lat[k]) / len(lat[k])
    out["busy_p90"], out["busy_p95"] = pct(lat["busy"], 0.9), pct(lat["busy"], 0.95)
    for kind, sel in (("busy", lambda p: p["target_rps"] >= busy_rps),
                      ("quiet", lambda p: p["target_rps"] < busy_rps)):
        mb = secs = 0.0
        for p in lg["phases"]:
            if sel(p):
                d = (p["end_unix_ns"] - p["start_unix_ns"]) / 1e9
                mb += runlib.batch_mb_per_s(run_dir, p["start_unix_ns"], p["end_unix_ns"]) * d
                secs += d
        out[kind + "_mbs"] = mb / secs if secs else float("nan")
    s = runlib.sched_stats(run_dir, lg["phases"][0]["start_unix_ns"], lg["phases"][-1]["end_unix_ns"])
    if s and "lc_cpus" in s:
        out["lc_cpus"] = s["lc_cpus"]
    path = os.path.join(run_dir, "sched_stats.csv")
    if os.path.exists(path):
        with open(path) as f:
            rows = list(csv.DictReader(f))
        if rows and "ml_raised" in rows[0]:
            fresh = sum(float(r["ml_fresh"]) for r in rows)
            out["ml_raised_pct"] = 100 * sum(float(r["ml_raised"]) for r in rows) / fresh if fresh else 0
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ref")
    ap.add_argument("--busy", type=float, default=2000)
    ap.add_argument("--onset-ms", type=float, default=250)
    ap.add_argument("--skip-reps", type=int, nargs="+", default=[])
    a = ap.parse_args()
    runs = aggregate.runs_by_config(a.dir, a.skip_reps)
    data = {n: [run_stats(d, a.busy, a.onset_ms) for d in runs[n]] for n in sorted(runs)}
    cols = ["quiet_p50", "quiet_p99", "busy_p50", "busy_p99", "onset_p50", "onset_p99",
            "quiet_mbs", "busy_mbs", "lc_cpus", "ml_raised_pct"]
    print(f"{a.dir}: median over reps (ms, MB/s); onset = first {a.onset_ms:.0f} ms of busy phases")
    print("  config     " + " ".join(f"{c:>11s}" for c in cols))
    for n, rs in data.items():
        vals = []
        for c in cols:
            xs = [r[c] for r in rs if c in r]
            vals.append(f"{statistics.median(xs):11.2f}" if xs else f"{'-':>11s}")
        print(f"  {n:10s} " + " ".join(vals))
    # Shares over a threshold are steadier than p99 when a few reps have
    # slow episodes: an episode moves p99 by 100s of ms but these by its
    # share of the requests.
    cols2 = [f"{k}>{t}" for k in ("quiet", "busy") for t in SLO_MS] + ["busy_p90", "busy_p95"]
    print("  % of requests slower than X ms, and busy p90/p95 (ms), median over reps")
    print("  config     " + " ".join(f"{c:>9s}" for c in cols2))
    for n, rs in data.items():
        print(f"  {n:10s} " + " ".join(f"{statistics.median(r[c] for r in rs):9.2f}" for c in cols2))
    if a.ref in data:
        print(f"  reps where config is better than {a.ref} (lower latency, higher MB/s):")
        for n, rs in data.items():
            if n == a.ref:
                continue
            wins = []
            for c in cols[:8]:
                better = (lambda x, y: x > y) if c.endswith("mbs") else (lambda x, y: x < y)
                wins.append(f"{c} {sum(better(x[c], y[c]) for x, y in zip(rs, data[a.ref]))}/{len(rs)}")
            print(f"    {n:10s} " + ", ".join(wins))


if __name__ == "__main__":
    main()
