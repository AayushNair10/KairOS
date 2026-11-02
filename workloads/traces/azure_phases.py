#!/usr/bin/env python3
"""Print a load-generator PHASES string from the Azure Functions 2021 trace.

The trace (Azure Public Dataset, AzureFunctionsInvocationTraceForTwoWeeksJan2021,
CC-BY 4.0) has one row per invocation: app, func, end_timestamp, duration,
in seconds from the trace start. It logs when each invocation ended, so
arrival = end_timestamp - duration.

One-hour windows of trace time are replayed one after another. Within a
window, arrivals are counted in --bin-s bins; each bin becomes one phase of
--phase-ms (time compression bin-s * 1000 / phase-ms, 100x by default).
The rate is scaled per window so that its busiest bin runs at --peak req/s,
the others in proportion, and none below --floor (a server still gets some
traffic when the trace has none). Within a phase the load generator sends
Poisson arrivals at that rate.

Windows: --hours H,... (hour H starts at trace time 3600*H), or --pick N
--seed S: N hours chosen at random among the full hours with at least the
median number of arrivals (sparser hours are mostly counting noise at 10 s
bins), replayed in trace order.

Usage: workloads/traces/azure_phases.py TRACE (--hours H,... | --pick N --seed S)
       [--bin-s 10] [--phase-ms 100] [--peak 3500] [--floor 300]
Prints PHASES on stdout and a summary per window on stderr.
"""
import argparse
import random
import sys


def arrivals(path):
    out = []
    with open(path) as f:
        next(f)  # header
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) != 4:
                continue  # the extracted file's last line is truncated
            try:
                out.append(float(p[2]) - float(p[3]))
            except ValueError:
                continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--hours")
    ap.add_argument("--pick", type=int)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--bin-s", type=float, default=10)
    ap.add_argument("--phase-ms", type=int, default=100)
    ap.add_argument("--peak", type=float, default=3500)
    ap.add_argument("--floor", type=float, default=300)
    a = ap.parse_args()

    arr = arrivals(a.trace)
    per_hour = int(3600 // a.bin_s)
    counts = [0] * (int(max(arr) // a.bin_s) + 1)
    for t in arr:
        if t >= 0:
            counts[int(t // a.bin_s)] += 1
    nhours = len(counts) // per_hour  # full hours only
    if a.hours:
        hours = [int(h) for h in a.hours.split(",")]
    elif a.pick:
        totals = [sum(counts[h * per_hour:(h + 1) * per_hour]) for h in range(nhours)]
        median = sorted(totals)[nhours // 2]
        hours = sorted(random.Random(a.seed).sample(
            [h for h in range(nhours) if totals[h] >= median], a.pick))
    else:
        raise SystemExit("give --hours or --pick")

    phases = []
    for h in hours:
        c = counts[h * per_hour:(h + 1) * per_hour]
        scale = a.peak / max(c)
        rates = [max(a.floor, round(x * scale)) for x in c]
        phases += [f"{a.phase_ms}ms@{r:.0f}" for r in rates]
        s = sorted(rates)
        print(f"hour {h}: {sum(c)} arrivals, busiest bin {max(c)}; replay "
              f"{len(c) * a.phase_ms / 1000:.0f} s, rate p50 {s[len(s) // 2]} p90 "
              f"{s[9 * len(s) // 10]} mean {sum(rates) / len(rates):.0f} req/s, "
              f"{100 * sum(r >= 2000 for r in rates) / len(rates):.0f}% of time >= 2000",
              file=sys.stderr)
    print(",".join(phases))


if __name__ == "__main__":
    main()
