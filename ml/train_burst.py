#!/usr/bin/env python3
"""Train and evaluate the LC demand predictor for scx_kairos --split.

Input: ml_trace.csv files written by scx_kairos --ml-trace (one LC demand
sample per period, in CPUs). For each sample time t the features are the
last K samples, newest first, and the scheduler's own demand EWMA:
    x = [d(t), d(t-1), ..., d(t-K+1), ewma(t)]
and the target is the largest sample of the next H periods:
    y = max(d(t+1), ..., d(t+H))
i.e. how much LC capacity the pool must have soon. scx_kairos's loader
builds the same features (ml_step() in scx_kairos.c).

Predictors compared (the plan asks for persistence and a moving average):
    persistence  y = d(t)
    mean         y = mean of the last K samples
    ewma         y = ewma(t), what the reactive pool sizing uses
    tree         a regression tree (CART, squared error), depth <= --depth
Pool decisions are scored as the scheduler makes them, with
n = ceil(demand + buffer) clamped to [1, CPUs]: "under" is the share of
samples where the pool would be smaller than the target needs, "over" the
mean number of CPUs given to LC beyond it. "max(ewma, tree)" is what the
BPF side does with a fresh prediction (it can only raise demand).
A bigger buffer also cuts "under" at the cost of "over", so the reactive
predictors are also scored with extra buffer, the smallest amount (in 0.05
CPU steps) that gives LC at least as many extra CPUs as max(ewma, tree):
the tree only helps if it beats them at the same cost.

The tree is plain Python (no numpy or scikit-learn on this VM) and is
written in the text format scx_kairos --ml-model reads.

Usage: ml/train_burst.py --train TRACE... --test TRACE... --out MODEL
"""
import argparse
import csv
import math


def load(path):
    """[(demand, ewma)] per sample from one ml_trace.csv."""
    with open(path) as f:
        return [(float(r["demand"]), float(r["ewma"])) for r in csv.DictReader(f)]


def samples(trace, k, h):
    """(x, y) pairs from one trace; windows never cross traces."""
    d = [s[0] for s in trace]
    out = []
    for t in range(k - 1, len(trace) - h):
        x = [d[t - i] for i in range(k)] + [trace[t][1]]
        out.append((x, max(d[t + 1:t + 1 + h])))
    return out


def fit_tree(X, y, depth, min_leaf):
    """CART regression tree. Returns nodes [feature, threshold, left, right,
    value] in pre-order (children after their parent); feature -1 = leaf.
    At each node it tries every feature and every cut between two distinct
    sorted values, keeping the one with the least total squared error,
    computed from running sums: SSE = sum(y^2) - sum(y)^2 / n per side."""
    nodes = []
    nfeat = len(X[0])

    def build(idx, d):
        me = len(nodes)
        nodes.append(None)
        ys = [y[j] for j in idx]
        n, tot, tot2 = len(ys), sum(ys), sum(v * v for v in ys)
        best = None
        if d < depth and n >= 2 * min_leaf:
            for f in range(nfeat):
                order = sorted(idx, key=lambda j: X[j][f])
                s = s2 = 0.0
                for c in range(1, n):
                    v = y[order[c - 1]]
                    s += v
                    s2 += v * v
                    if c < min_leaf or n - c < min_leaf:
                        continue
                    a, b = X[order[c - 1]][f], X[order[c]][f]
                    if a == b:
                        continue
                    sse = (s2 - s * s / c) + ((tot2 - s2) - (tot - s) ** 2 / (n - c))
                    if best is None or sse < best[0]:
                        best = (sse, f, (a + b) / 2, order[:c], order[c:])
        if best is None or best[0] >= tot2 - tot * tot / n:
            nodes[me] = [-1, 0.0, -1, -1, tot / n]
            return me
        _, f, thr, left, right = best
        nodes[me] = [f, thr, 0, 0, tot / n]
        nodes[me][2] = build(left, d + 1)
        nodes[me][3] = build(right, d + 1)
        return me

    build(list(range(len(X))), 0)
    return nodes


def predict(nodes, x):
    i = 0
    while nodes[i][0] >= 0:
        i = nodes[i][2] if x[nodes[i][0]] <= nodes[i][1] else nodes[i][3]
    return nodes[i][4]


def pool(demand, buffer, cpus):
    return min(cpus, max(1, math.ceil(demand + buffer - 1e-9)))


def score(pairs, pred, buffer, cpus):
    """MAE, under-provisioned share, mean extra LC CPUs."""
    mae = under = over = 0.0
    for (x, y), p in zip(pairs, pred):
        mae += abs(p - y)
        need, got = pool(y, buffer, cpus), pool(p, buffer, cpus)
        under += got < need
        over += max(0, got - need)
    n = len(pairs)
    return mae / n, under / n, over / n


def evaluate(name, pairs, nodes, k, buffer, cpus):
    preds = {
        "persistence": [x[0] for x, _ in pairs],
        "mean": [sum(x[:k]) / k for x, _ in pairs],
        "ewma": [x[k] for x, _ in pairs],
        "tree": [predict(nodes, x) for x, _ in pairs],
    }
    preds["max(ewma, tree)"] = [max(a, b) for a, b in zip(preds["ewma"], preds["tree"])]
    print(f"{name}: {len(pairs)} samples")
    print(f"  {'predictor':24s} {'MAE':>6s} {'under':>7s} {'over':>6s}")
    for p, v in preds.items():
        mae, under, over = score(pairs, v, buffer, cpus)
        print(f"  {p:24s} {mae:6.3f} {100 * under:6.1f}% {over:6.3f}")
    target_over = score(pairs, preds["max(ewma, tree)"], buffer, cpus)[2]
    for p in ("persistence", "ewma"):
        for step in range(1, 61):
            extra = step * 0.05
            mae, under, over = score(pairs, [v + extra for v in preds[p]], buffer, cpus)
            if over >= target_over:
                print(f"  {p + f' +{extra:.2f} buffer':24s} {mae:6.3f} {100 * under:6.1f}% {over:6.3f}")
                break


def describe(nodes, k, i=0, depth=0):
    """The tree as indented text, for reading."""
    f, thr, left, right, value = nodes[i]
    pad = "  " * depth
    if f < 0:
        return [f"{pad}-> {value:.2f} CPUs"]
    name = f"d(t-{f})" if 0 < f < k else ("d(t)" if f == 0 else "ewma")
    return ([f"{pad}if {name} <= {thr:.3f}:"] + describe(nodes, k, left, depth + 1)
            + [f"{pad}else:"] + describe(nodes, k, right, depth + 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--test", nargs="*", default=[])
    ap.add_argument("--out")
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--h", type=int, default=4)
    ap.add_argument("--period-ms", type=int, default=50)
    ap.add_argument("--depth", type=int, default=5)
    ap.add_argument("--min-leaf", type=int, default=50)
    ap.add_argument("--buffer", type=float, default=0.5, help="scx_kairos --lc-buffer-pct / 100")
    ap.add_argument("--cpus", type=int, default=3, help="split CPUs")
    a = ap.parse_args()

    train = [p for path in a.train for p in samples(load(path), a.k, a.h)]
    nodes = fit_tree([x for x, _ in train], [y for _, y in train], a.depth, a.min_leaf)
    print(f"tree: {len(nodes)} nodes, depth <= {a.depth}")
    print("\n".join(describe(nodes, a.k)))
    evaluate("train (in-sample)", train, nodes, a.k, a.buffer, a.cpus)
    for path in a.test:
        evaluate(f"test {path}", samples(load(path), a.k, a.h), nodes, a.k, a.buffer, a.cpus)
    if a.out:
        with open(a.out, "w") as f:
            f.write(f"kairos-tree 1\nk {a.k} h {a.h} period_ms {a.period_ms}\n")
            f.write(f"nodes {len(nodes)}\n")
            for feat, thr, left, right, value in nodes:
                f.write(f"{feat} {thr:.6f} {left} {right} {value:.6f}\n")
        print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
