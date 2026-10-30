#!/usr/bin/env python3
"""Split request latency into its parts, per configuration and target rate.

Needs requests.csv with the server's recv_unix_ns and wake_ns (from commit
dbfae96 on). Each request's latency (done - intended) is the sum of:
  lag      intended -> sent (the load generator's own delay)
  in       sent -> the server's handler has the request (client, loopback,
           Go's network poller, gRPC's reader, the handler goroutine)
  queue    handler -> a worker starts it
  service  the worker's work
  wake     worker finished -> the handler goroutine runs again
  out      handler returns -> the load generator has the reply
"in" and "out" compare the server's wall clock with the load generator's
(start_unix_ns + its monotonic offsets), so they can be off by clock
adjustments during a run (small next to milliseconds).

For each part: the median over reps of its p50 and p99 over the phase's
requests, then the mean of each part over each rep's slowest 1% of
requests (median over reps): which parts make the tail.

Usage: analysis/split.py results/<date>-<name> [config ...]
"""
import csv
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402

PARTS = ("lag", "in", "queue", "service", "wake", "out")


def parts_by_rate(run_dir):
    """{rate: [(latency, lag, in, queue, service, wake, out), ...]} in ms."""
    lg = runlib.load_summary(run_dir)
    st = lg["start_unix_ns"]
    rate_of = {p["phase"]: p["target_rps"] for p in lg["phases"]}
    out = {}
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            if r["code"] != "0":
                continue
            sent, done = int(r["sent_ns"]), int(r["done_ns"])
            recv = int(r["recv_unix_ns"]) - st
            q, s, w = int(r["queue_ns"]), int(r["service_ns"]), int(r["wake_ns"])
            v = (int(r["latency_ns"]), sent - int(r["intended_ns"]), recv - sent, q, s, w,
                 done - recv - q - s - w)
            out.setdefault(rate_of[int(r["phase"])], []).append(tuple(x / 1e6 for x in v))
    return out


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def main():
    runs = aggregate.runs_by_config(sys.argv[1])
    names = sys.argv[2:] or sorted(runs)
    data = {n: [parts_by_rate(d) for d in runs[n]] for n in names}
    for rate in sorted(next(iter(data.values()))[0]):
        print(f"== {rate:.0f} req/s, ms (median over reps)")
        print("  config     latency p50/p99   " + "  ".join(f"{p:>10s}" for p in PARTS)
              + "   | slowest 1%: mean latency = " + " + ".join(PARTS))
        for n in names:
            reps = [d[rate] for d in data[n]]
            med = lambda f: statistics.median(f(rows) for rows in reps)  # noqa: E731
            lat = f"{med(lambda r: pct([x[0] for x in r], .5)):6.1f}/{med(lambda r: pct([x[0] for x in r], .99)):6.1f}"
            cols = []
            for i in range(1, 7):
                cols.append(f"{med(lambda r: pct([x[i] for x in r], .5)):4.2f}/"
                            f"{med(lambda r: pct([x[i] for x in r], .99)):<5.1f}")

            def tail_mean(rows, i):
                rows = sorted(rows)[int(0.99 * len(rows)):]
                return sum(x[i] for x in rows) / len(rows)
            tail = [med(lambda r: tail_mean(r, i)) for i in range(7)]
            print(f"  {n:9s} {lat}   " + "  ".join(f"{c:>10s}" for c in cols)
                  + f"   | {tail[0]:6.1f} = " + " + ".join(f"{t:.1f}" for t in tail[1:]))


if __name__ == "__main__":
    main()
