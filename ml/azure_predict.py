#!/usr/bin/env python3
"""Offline predictability check of the Azure Functions 2021 trace at 1 s
resolution: can a learned predictor size scx_kairos's LC pool better than
simple ones, before any machine time is spent on it (RESEARCH_LOG
section 9, ML)?

The series is what workloads/traces/azure_phases.py --bin-s 1 would
replay (10x compression, one 100 ms phase per second of trace): arrivals
counted per second, each hour scaled so its busiest second runs at --peak
req/s, none below --floor. LC demand in CPUs = rate x --cpu-ms-per-req /
1000 (about 1 ms in E2a's kairos runs). One sample per phase, so the
predictor's 500 ms of history and 200 ms horizon (ml/train_burst.py at 50
ms samples) are K = 5 and H = 2 phases. Timer-triggered functions put 5.7%
of all arrivals in second :00, a period of P = 60 phases (6 s of replay).
The demand is the replayed rate itself, without the noise of measuring it
in the scheduler, so this is an optimistic test for every predictor.

Predictors of y(t) = the largest demand over the next H phases:
  persistence      d(t)
  mean             mean of the last K phases
  ewma             the scheduler's demand EWMA (weight 1/64 per 1 ms tick,
                   so 1 - (63/64)^100 per 100 ms phase)
  seasonal         y one period earlier: max(d(t+1-P), ..., d(t+H-P))
  seasonal mean    the mean of that over the last M periods
  tree             CART regression tree (ml/train_burst.py), depth <=
                   --depth, on [last K phases, ewma, seasonal value of each
                   of the last M periods]
  max(ewma, tree)  what the BPF side does with a fresh prediction (it can
                   only raise the demand)
  max(ewma, seasonal mean)  the same with the seasonal mean (no learning)
Trained on --train-pick hours drawn at random (--train-seed) from the hours
with at least the median number of arrivals, the test hours left out;
tested on --test hours (default E2a's 36, 54, 284) one by one. Scored as in
ml/train_burst.py: MAE (CPUs), and the pool ceil(demand + --buffer) in
[1, --cpus] against the pool y needs: under = share of samples where the
pool is too small, over = mean LC CPUs beyond what y needs; "under :00"
counts only the samples whose horizon holds a second :00, the onsets a
predictor could foresee and a reactive one can't. Then every simple
predictor at the cost of max(ewma, tree): with the extra buffer (negative
or positive, 0.01 CPU steps) at which its "over" reaches the tree's,
"under" interpolated linearly between the two steps around it (a pool
alternating between those two buffers would get it). Pools are whole
CPUs, so a fixed step can overshoot the cost by far. The tree only helps
if it is under-provisioned less often than every simple predictor at the
same cost.

Usage: ml/azure_predict.py TRACE [--test 36,54,284] [--train-pick 24]
       [--train-seed 2] [--peak 3500] [--cpus 8] [--buffer 0.5] [--depth 5]
"""
import argparse
import os
import random
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "workloads", "traces"))
import azure_phases  # noqa: E402
import train_burst  # noqa: E402

K, H, P = 5, 2, 60
EWMA_W = 1 - (63 / 64) ** 100


def hour_demand(counts, h, a):
    """Demand per phase (CPUs) for trace hour h, scaled as azure_phases.py."""
    c = counts[h * 3600:(h + 1) * 3600]
    scale = a.peak / max(c)
    return [max(a.floor, round(x * scale)) * a.cpu_ms_per_req / 1000 for x in c]


def samples(d, m):
    """(x, y) pairs and their phases t from one hour, x = [d(t), ...,
    d(t-K+1), ewma(t), seasonal(t, 1), ..., seasonal(t, m)] with
    seasonal(t, i) = max(d(t+1-iP), ..., d(t+H-iP)); y = max(d(t+1), ...,
    d(t+H)). The first m periods only warm up the features, so every sample
    has all of them. Phase t is second t of the hour, and hours start on a
    whole minute of trace time, so (t + j) % P == 0 is a second :00."""
    e, ew = d[0], []
    for v in d:
        e += EWMA_W * (v - e)
        ew.append(e)
    out, ts = [], []
    for t in range(m * P, len(d) - H):
        x = [d[t - i] for i in range(K)] + [ew[t]]
        x += [max(d[t + 1 - i * P:t + 1 + H - i * P]) for i in range(1, m + 1)]
        out.append((x, max(d[t + 1:t + 1 + H])))
        ts.append(t)
    return out, ts


def acf(x, lag):
    mu = sum(x) / len(x)
    v = sum((a - mu) ** 2 for a in x)
    return sum((x[i] - mu) * (x[i + lag] - mu) for i in range(len(x) - lag)) / v if v else 0.0


def onset(t):
    """The horizon of the sample at phase t holds a second :00."""
    return any((t + j) % P == 0 for j in range(1, H + 1))


def score(pairs, ts, pred, buffer, cpus):
    """MAE, under, over (train_burst.score) and under at :00 onsets."""
    mae, under, over = train_burst.score(pairs, pred, buffer, cpus)
    on = [i for i, t in enumerate(ts) if onset(t)]
    u0 = train_burst.score([pairs[i] for i in on], [pred[i] for i in on], buffer, cpus)[1]
    return mae, under, over, u0


def at_cost(pairs, ts, pred, buffer, cpus, target):
    """pred + extra buffer at the cost (over) target: the extra buffer and
    the under, under-at-:00 interpolated between the two 0.01 CPU steps
    around it. over grows with the extra buffer, so a bisection finds the
    first step at or above target."""
    def at(step):
        return score(pairs, ts, [v + step / 100 for v in pred], buffer, cpus)
    lo, hi = -100, 300  # extra buffer -1 ... +3 CPUs
    if at(lo)[2] >= target or at(hi)[2] < target:
        return None
    while hi - lo > 1:  # at(lo) below target, at(hi) at or above
        mid = (lo + hi) // 2
        lo, hi = (mid, hi) if at(mid)[2] < target else (lo, mid)
    a, b = at(lo), at(hi)
    f = (target - a[2]) / (b[2] - a[2])
    return ((lo + f) / 100, a[1] + f * (b[1] - a[1]), a[3] + f * (b[3] - a[3]))


def evaluate(pairs, ts, nodes, m, cpus, buffer, matched=True):
    """Rows (name, MAE, under, over, under at :00): every predictor, then
    (with matched) the simple ones at the cost of max(ewma, tree)."""
    preds = {
        "persistence": [x[0] for x, _ in pairs],
        "mean": [sum(x[:K]) / K for x, _ in pairs],
        "ewma": [x[K] for x, _ in pairs],
        "seasonal": [x[K + 1] for x, _ in pairs],
        f"seasonal mean (M={m})": [sum(x[K + 1:]) / m for x, _ in pairs],
        "tree": [train_burst.predict(nodes, x) for x, _ in pairs],
    }
    sm = f"seasonal mean (M={m})"
    preds[f"max(ewma, {sm})"] = [max(a, b) for a, b in zip(preds["ewma"], preds[sm])]
    preds["max(ewma, tree)"] = [max(a, b) for a, b in zip(preds["ewma"], preds["tree"])]
    rows = [(p, *score(pairs, ts, v, buffer, cpus)) for p, v in preds.items()]
    target = rows[-1][3]
    for p in [q for q in preds if "tree" not in q] if matched else []:
        r = at_cost(pairs, ts, preds[p], buffer, cpus, target)
        if r:
            rows.append((f"{p} {r[0]:+.2f}", float("nan"), r[1], target, r[2]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--test", default="36,54,284")
    ap.add_argument("--train-pick", type=int, default=24)
    ap.add_argument("--train-seed", type=int, default=2)
    ap.add_argument("--peak", type=float, default=3500)
    ap.add_argument("--floor", type=float, default=300)
    ap.add_argument("--cpu-ms-per-req", type=float, default=1.0)
    ap.add_argument("--m", type=int, default=5, help="seasonal periods")
    ap.add_argument("--cpus", default="8")
    ap.add_argument("--buffer", type=float, default=0.5)
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("--min-leaf", type=int, default=50)
    a = ap.parse_args()

    arr = azure_phases.arrivals(a.trace)
    counts = [0] * (int(max(arr)) + 1)
    for t in arr:
        if t >= 0:
            counts[int(t)] += 1
    nhours = len(counts) // 3600
    totals = [sum(counts[h * 3600:(h + 1) * 3600]) for h in range(nhours)]
    median = sorted(totals)[nhours // 2]
    test = [int(h) for h in a.test.split(",")]
    eligible = [h for h in range(nhours) if totals[h] >= median and h not in test]
    train = sorted(random.Random(a.train_seed).sample(eligible, a.train_pick))
    print(f"train hours ({len(train)}): {','.join(map(str, train))}")

    for h in test:
        d = hour_demand(counts, h, a)
        c = counts[h * 3600:(h + 1) * 3600]
        print(f"test hour {h}: {totals[h]} arrivals; demand mean {statistics.mean(d):.2f} CPUs, "
              f"{100 * sum(v <= a.floor * a.cpu_ms_per_req / 1000 for v in d) / len(d):.0f}% of "
              f"phases at the floor; acf lag 1 {acf(d, 1):+.2f}, lag {P} {acf(d, P):+.2f}; "
              f"arrivals/s at :00 {statistics.mean(c[0::60]):.1f}, other seconds "
              f"{statistics.mean(v for i, v in enumerate(c) if i % 60):.1f}")

    tr, trt = [], []
    for h in train:
        pairs, ts = samples(hour_demand(counts, h, a), a.m)
        tr += pairs
        trt += ts
    nodes = train_burst.fit_tree([x for x, _ in tr], [y for _, y in tr], a.depth, a.min_leaf)
    used = sorted({n[0] for n in nodes if n[0] >= 0})
    names = [f"d(t-{i})" if i else "d(t)" for i in range(K)] + ["ewma"] + [
        f"seasonal{i}" for i in range(1, a.m + 1)]
    print(f"tree: {len(nodes)} nodes, depth <= {a.depth}, {len(tr)} training samples; "
          f"features used: {', '.join(names[f] for f in used)}")
    for cpus in (int(x) for x in a.cpus.split(",")):
        sets = [("train (in-sample)", tr, trt, False)] + [
            (f"test hour {h}", *samples(hour_demand(counts, h, a), a.m), True) for h in test]
        for name, pairs, ts, matched in sets:
            print(f"== {name}, {cpus} CPUs, buffer {a.buffer}: {len(pairs)} samples, "
                  f"{sum(map(onset, ts))} with a :00 in the horizon")
            print(f"  {'predictor':38s} {'MAE':>6s} {'under':>7s} {'over':>6s} {'under :00':>10s}")
            for p, mae, under, over, u0 in evaluate(pairs, ts, nodes, a.m, cpus, a.buffer, matched):
                m = f"{mae:6.3f}" if mae == mae else f"{'-':>6s}"  # no MAE at a matched cost
                print(f"  {p:38s} {m} {100 * under:6.1f}% {over:6.3f} {100 * u0:9.1f}%")


if __name__ == "__main__":
    main()
