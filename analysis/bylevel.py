#!/usr/bin/env python3
"""Compare configurations by load level, for patterns with many phases at
many rates (e.g. the Azure trace replay).

Requests are pooled per run by their phase's target rate into levels
(--levels 1000,2000: < 1000, 1000-2000, >= 2000 req/s), plus all requests
together. Per level: p50, p99 (failed requests count as infinitely slow)
and the share of requests slower than 2/5/10 ms; for all requests also
p99.9 and batch MB/s over the whole load. With --windows N the phases are
also split into N equal consecutive parts (one per trace window). Median
over reps; then, against --ref, in how many reps each config was better.

Usage: analysis/bylevel.py results/<date>-<name> [--ref CONFIG]
       [--levels 1000,2000] [--windows N] [--skip-reps N ...]
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


def pct(xs, q):
    if not xs:
        return float("nan")
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def stats(lat, extra=False):
    lat.sort()
    out = {"n": len(lat), "p50": pct(lat, 0.5), "p99": pct(lat, 0.99)}
    for t in SLO_MS:
        out[f">{t}"] = 100 * sum(x > t for x in lat) / len(lat) if lat else float("nan")
    if extra:
        out["p999"] = pct(lat, 0.999)
    return out


def run_stats(run_dir, levels, windows):
    lg = runlib.load_summary(run_dir)
    rates = {p["phase"]: p["target_rps"] for p in lg["phases"]}
    nph = len(lg["phases"])
    names = [f"<{levels[0]}"] + [f"{a}-{b}" for a, b in zip(levels, levels[1:])] + [f">={levels[-1]}"]
    lat = {k: [] for k in names + ["all"] + [f"w{i}" for i in range(windows)]}
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            ph = int(r["phase"])
            v = int(r["latency_ns"]) / 1e6 if r["code"] == "0" else float("inf")
            lvl = sum(rates[ph] >= x for x in levels)
            lat[names[lvl]].append(v)
            lat["all"].append(v)
            if windows:
                lat[f"w{ph * windows // nph}"].append(v)
    out = {k: stats(v, extra=(k == "all" or k.startswith("w"))) for k, v in lat.items()}
    p0, p1 = lg["phases"][0], lg["phases"][-1]
    out["all"]["mbs"] = runlib.batch_mb_per_s(run_dir, p0["start_unix_ns"], p1["end_unix_ns"])
    for i in range(windows):
        a, b = lg["phases"][i * nph // windows], lg["phases"][(i + 1) * nph // windows - 1]
        out[f"w{i}"]["mbs"] = runlib.batch_mb_per_s(run_dir, a["start_unix_ns"], b["end_unix_ns"])
    return names, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ref")
    ap.add_argument("--levels", default="1000,2000")
    ap.add_argument("--windows", type=int, default=0)
    ap.add_argument("--skip-reps", type=int, nargs="+", default=[])
    a = ap.parse_args()
    levels = [float(x) for x in a.levels.split(",")]
    runs = aggregate.runs_by_config(a.dir, a.skip_reps)
    data, names = {}, None
    for n in sorted(runs):
        data[n] = []
        for d in runs[n]:
            names, s = run_stats(d, levels, a.windows)
            data[n].append(s)
    med = lambda rs, k, c: statistics.median(r[k][c] for r in rs)  # noqa: E731

    print(f"{a.dir}: median over reps (ms; >X = % of requests slower than X ms)")
    for k in names:
        print(f"== target rate {k} req/s ({statistics.median(r[k]['n'] for r in next(iter(data.values()))):.0f} requests per run)")
        cols = ["p50", "p99"] + [f">{t}" for t in SLO_MS]
        print("  config     " + " ".join(f"{c:>8s}" for c in cols))
        for n, rs in data.items():
            print(f"  {n:10s} " + " ".join(f"{med(rs, k, c):8.2f}" for c in cols))
    parts = ["all"] + [f"w{i}" for i in range(a.windows)]
    for k in parts:
        print(f"== {'all requests' if k == 'all' else 'window ' + k[1:]}, batch MB/s over the same time")
        cols = ["p50", "p99", "p999"] + [f">{t}" for t in SLO_MS] + ["mbs"]
        print("  config     " + " ".join(f"{c:>8s}" for c in cols))
        for n, rs in data.items():
            print(f"  {n:10s} " + " ".join(f"{med(rs, k, c):8.2f}" for c in cols))
    if a.ref in data:
        print(f"== reps where config is better than {a.ref} (lower latency, higher MB/s)")
        for n, rs in data.items():
            if n == a.ref:
                continue
            wins = []
            for k, c in [(k, "p50") for k in names] + [("all", "p99"), ("all", ">5"), ("all", "mbs")]:
                better = (lambda x, y: x > y) if c == "mbs" else (lambda x, y: x < y)
                wins.append(f"{k} {c} {sum(better(x[k][c], y[k][c]) for x, y in zip(rs, data[a.ref]))}/{len(rs)}")
            print(f"  {n:10s} " + ", ".join(wins))


if __name__ == "__main__":
    main()
