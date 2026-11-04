#!/usr/bin/env python3
"""Statistical tests for the final evaluation: every configuration against
a reference configuration (--ref), paired by repetition.

Metrics per run:
  --by burst (burst-length sweep, byburst.py): per burst length L, busy
    p50, busy share of requests over 5 ms, batch MB/s over the block; over
    all lengths together, busy and quiet p50, busy share over 5 ms, and
    batch MB/s over the whole load.
  --by level (many-rate patterns such as the Azure replay, bylevel.py):
    per load level, p50 and share over 5 ms; over all requests, p50, p99,
    p99.9, share over 5 ms and batch MB/s.
Failed requests count as infinitely slow, as in byburst.py and bylevel.py.
--by is guessed from the phases when not given: two target rates mean a
burst sweep.

run.sh interleaves the configurations within a repetition, so runs with
the same rep number ran close in time; they are compared as a pair. For
each metric and each configuration C other than the reference, with
d_i = C's value - ref's value in rep i:
  diff [95% CI]  median of d, and a bootstrap 95% CI for it (percentile
                 method, 10,000 resamples of the reps with replacement,
                 seeded by metric and config, so the output is repeatable)
  %              median of d as a share of ref's median
  better         in how many reps C was better than ref (lower latency or
                 share, more MB/s)
  p              two-sided Wilcoxon signed-rank test of d, exact: the
                 rank sum is compared with its distribution over all 2^n
                 sign patterns (ties get average ranks; zero differences
                 are dropped)
  holm           p adjusted with Holm's method within the metric, i.e.
                 over that metric's comparisons with ref; * = holm < 0.05
With 10 reps the smallest exact p is 2/1024 = 0.002, so Holm can only
reject in families of at most 25 tests: one family per metric keeps it
there, and the per-L and per-level metrics are reported, not pooled into
one family.

Usage: analysis/stats.py results/<date>-<name> --ref CONFIG [CONFIG ...]
       [--by burst|level] [--levels 1000,2000] [--configs C ...]
       [--skip-reps N ...] [--csv FILE]
(several --ref configs print one section each; --configs picks and orders
the configurations; --csv writes every comparison as one row)
"""
import argparse
import csv
import os
import random
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import byburst  # noqa: E402
import bylevel  # noqa: E402
import runlib  # noqa: E402

BOOT = 10000
ALPHA = 0.05


def burst_metrics(run_dir):
    """[(name, unit, higher is better, value)] for a burst-sweep run."""
    lengths, s = byburst.run_stats(run_dir)
    ph = runlib.load_summary(run_dir)["phases"]
    out = [("busy p50, all L", "ms", False, s[("busy", "all")]["p50"]),
           ("busy >5 ms, all L", "%", False, s[("busy", "all")][">5"]),
           ("quiet p50, all L", "ms", False, s[("quiet", "all")]["p50"]),
           ("batch, whole load", "MB/s", True,
            runlib.batch_mb_per_s(run_dir, ph[0]["start_unix_ns"], ph[-1]["end_unix_ns"]))]
    for L in lengths:
        out += [(f"busy p50, L {L} ms", "ms", False, s[("busy", L)]["p50"]),
                (f"busy >5 ms, L {L} ms", "%", False, s[("busy", L)][">5"]),
                (f"batch, L {L} ms block", "MB/s", True, s[("block", L)]["mbs"])]
    return out


def level_metrics(run_dir, levels):
    """[(name, unit, higher is better, value)] for a many-rate run."""
    names, s = bylevel.run_stats(run_dir, levels, 0)
    a = s["all"]
    out = [("p50, all", "ms", False, a["p50"]), ("p99, all", "ms", False, a["p99"]),
           ("p99.9, all", "ms", False, a["p999"]), (">5 ms, all", "%", False, a[">5"]),
           ("batch, whole load", "MB/s", True, a["mbs"])]
    for n in names:
        out += [(f"p50, {n} req/s", "ms", False, s[n]["p50"]),
                (f">5 ms, {n} req/s", "%", False, s[n][">5"])]
    return out


def diff(x, y):
    """x - y, with inf - inf = 0 (both runs had over half their requests
    fail, at this metric they are equal)."""
    return 0.0 if x == y else x - y


def wilcoxon(d):
    """Two-sided exact p of the Wilcoxon signed-rank test of the paired
    differences d."""
    d = [x for x in d if x != 0]
    n = len(d)
    if n == 0:
        return 1.0
    order = sorted(range(n), key=lambda i: abs(d[i]))
    # Twice the rank, so the average rank of a tie stays an integer: tied
    # positions i..j (0-based) share rank (i + 1 + j + 1) / 2.
    rank2 = [0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and abs(d[order[j + 1]]) == abs(d[order[i]]):
            j += 1
        for k in range(i, j + 1):
            rank2[order[k]] = i + j + 2
        i = j + 1
    w = sum(r for r, x in zip(rank2, d) if x > 0)
    # Under the null hypothesis each difference is positive or negative
    # with probability 1/2, independently. counts[s] = number of the 2^n
    # sign patterns whose positive (doubled) ranks sum to s, built one rank
    # at a time.
    counts = {0: 1}
    for r in rank2:
        nxt = dict(counts)
        for s, c in counts.items():
            nxt[s + r] = nxt.get(s + r, 0) + c
        counts = nxt
    lo = sum(c for s, c in counts.items() if s <= w)
    hi = sum(c for s, c in counts.items() if s >= w)
    return min(1.0, 2 * min(lo, hi) / 2 ** n)


def holm(ps):
    """Holm-adjusted p values, in the order given: the k-th smallest p is
    multiplied by (m - k + 1), and the result kept non-decreasing."""
    m = len(ps)
    out, run = [1.0] * m, 0.0
    for k, i in enumerate(sorted(range(m), key=lambda i: ps[i])):
        run = max(run, min(1.0, (m - k) * ps[i]))
        out[i] = run
    return out


def boot_ci(d, seed):
    """Percentile bootstrap 95% CI of the median of d."""
    rng = random.Random(seed)
    meds = sorted(statistics.median(rng.choices(d, k=len(d))) for _ in range(BOOT))
    return meds[round(0.025 * (BOOT - 1))], meds[round(0.975 * (BOOT - 1))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ref", nargs="+", required=True)
    ap.add_argument("--by", choices=("burst", "level"))
    ap.add_argument("--levels", default="1000,2000")
    ap.add_argument("--configs", nargs="+")
    ap.add_argument("--skip-reps", type=int, nargs="+", default=[])
    ap.add_argument("--csv")
    a = ap.parse_args()
    runs = aggregate.runs_by_config(a.dir, a.skip_reps)
    names = a.configs or sorted(runs)
    for r in a.ref:
        if r not in runs:
            raise SystemExit(f"no runs of --ref {r} in {a.dir}")
    by = a.by
    if by is None:
        ph = runlib.load_summary(runs[names[0]][0])["phases"]
        by = "burst" if len({p["target_rps"] for p in ph}) == 2 else "level"
    levels = [int(x) for x in a.levels.split(",")]

    # vals[config][rep] = {metric: value}; meta = [(metric, unit, higher)]
    vals, meta = {}, None
    for n in set(names) | set(a.ref):
        vals[n] = {}
        for d in runs[n]:
            rep = int(re.fullmatch(r".+-r(\d+)", os.path.basename(d)).group(1))
            ms = burst_metrics(d) if by == "burst" else level_metrics(d, levels)
            meta = meta or [(m, u, h) for m, u, h, _ in ms]
            vals[n][rep] = {m: v for m, _, _, v in ms}

    fmt = {"ms": "{:.2f}", "%": "{:.1f}", "MB/s": "{:.0f}"}
    sfmt = {u: f.replace("{:", "{:+") for u, f in fmt.items()}  # with a sign
    rows = []
    print(f"{a.dir}: metrics by {by}; paired by rep; {BOOT} bootstrap resamples; "
          f"Holm within each metric; * = Holm-adjusted p < {ALPHA}")
    for ref in a.ref:
        others = [n for n in names if n != ref]
        for metric, unit, higher in meta:
            f = fmt[unit]
            refv = [vals[ref][r][metric] for r in sorted(vals[ref])]
            res = []
            for n in others:
                reps = sorted(set(vals[n]) & set(vals[ref]))
                x = [vals[n][r][metric] for r in reps]
                d = [diff(vals[n][r][metric], vals[ref][r][metric]) for r in reps]
                better = sum((v > 0) if higher else (v < 0) for v in d)
                med = statistics.median(d)
                lo, hi = boot_ci(d, f"{metric}|{n}|{ref}")
                res.append({"metric": metric, "unit": unit, "ref": ref, "config": n,
                            "reps": len(reps), "median": statistics.median(x),
                            "min": min(x), "max": max(x), "ref_median": statistics.median(refv),
                            "diff": med, "ci_lo": lo, "ci_hi": hi,
                            "pct": 100 * med / statistics.median(refv)
                            if statistics.median(refv) else float("nan"),
                            "better": better, "p": wilcoxon(d)})
            for r, h in zip(res, holm([r["p"] for r in res])):
                r["holm"] = h
            rows += res
            print(f"== {metric} ({unit}; {'higher' if higher else 'lower'} is better); "
                  f"{ref}: " + f.format(statistics.median(refv)) + " ["
                  + f.format(min(refv)) + "-" + f.format(max(refv)) + "]")
            print(f"  {'config':11} {'median [min-max]':>22} {'diff vs ' + ref + ' [95% CI]':>30}"
                  f" {'%':>6} {'better':>7} {'p':>7} {'holm':>7}")
            for r in res:
                span = (f.format(r["median"]) + " [" + f.format(r["min"]) + "-"
                        + f.format(r["max"]) + "]")
                sf = sfmt[unit]
                ci = (sf.format(r["diff"]) + " [" + sf.format(r["ci_lo"]) + ", "
                      + sf.format(r["ci_hi"]) + "]")
                print(f"  {r['config']:11} {span:>22} {ci:>30} {r['pct']:+5.0f}% "
                      f"{r['better']:>3}/{r['reps']:<3} {r['p']:7.4f} {r['holm']:7.4f}"
                      + (" *" if r["holm"] < ALPHA else ""))
    if a.csv:
        with open(a.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
