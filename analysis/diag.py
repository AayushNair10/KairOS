#!/usr/bin/env python3
"""Where do the server's threads wait? Summarize scx_kairos --diag output.

For each configuration and each target rate (phases with the same rate are
pooled), from diag.csv: per task group, the waits for a CPU per request,
split by why the task was waiting (woken / preempted / slice ran out /
other), the waiting time per request, how many of those waits lasted 5 ms
or more, and CPU time. From the server's "schedlat" lines: how long
goroutines waited to run inside Go. Values are medians over reps of each
run's totals over the phases.

"Per request" divides by the requests the load generator completed in the
same phases, so it is a rate, not a per-request trace: a request is not
known to cause any particular wait.

Usage: analysis/diag.py results/<date>-<name> [config ...]
"""
import csv
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate  # noqa: E402
import runlib  # noqa: E402

KINDS = ("wakeup", "preempted", "slice_end", "other")
# Bucket columns of diag.csv / schedlat counts that are >= 5 ms and >= 20 ms.
GE5 = slice(5, None)   # lt10000us, lt20000us, ge20000us
GE20 = slice(7, None)  # ge20000us


def windows(lg):
    """{rate: [(start_ns, end_ns), ...]} of the load generator's phases."""
    out = {}
    for p in lg["phases"]:
        out.setdefault(p["target_rps"], []).append((p["start_unix_ns"], p["end_unix_ns"]))
    return out


def inside(t_ns, wins):
    # A row covers the second before its timestamp: count it if that whole
    # second is inside a phase (as runlib.sched_stats does).
    return any(s + 1e9 <= t_ns <= e for s, e in wins)


def run_totals(run_dir):
    """{rate: {"req": n, "secs": n, (group, kind): [count, ms, buckets...],
    "schedlat": [buckets...]}} summed over that rate's phases."""
    lg = runlib.load_summary(run_dir)
    out = {}
    for rate, wins in windows(lg).items():
        tot = out.setdefault(rate, {"req": 0, "secs": 0, "schedlat": None})
        tot["req"] = sum(p["requests"] - p["errors"] for p in lg["phases"]
                         if p["target_rps"] == rate)
    path = os.path.join(run_dir, "diag.csv")
    if os.path.exists(path):
        with open(path) as f:
            for row in csv.reader(f):
                if row[0] == "unix_s":
                    continue
                t = float(row[0]) * 1e9
                for rate, wins in windows(lg).items():
                    if inside(t, wins):
                        v = [float(x) for x in row[3:]]
                        acc = out[rate].setdefault((row[1], row[2]), [0.0] * len(v))
                        out[rate][(row[1], row[2])] = [a + b for a, b in zip(acc, v)]
                        if row[1] == "other" and row[2] == "run":
                            out[rate]["secs"] += 1
    with open(os.path.join(run_dir, "server.log"), errors="replace") as f:
        for line in f:
            m = re.search(r"schedlat unix_ns=(\d+) counts=([\d,]+)", line)
            if not m:
                continue
            c = [int(x) for x in m.group(2).split(",")]
            for rate, wins in windows(lg).items():
                if inside(int(m.group(1)), wins):
                    acc = out[rate]["schedlat"] or [0] * len(c)
                    out[rate]["schedlat"] = [a + b for a, b in zip(acc, c)]
    return out


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else float("nan")


def main():
    out_dir = sys.argv[1]
    runs = aggregate.runs_by_config(out_dir)
    names = sys.argv[2:] or sorted(runs)
    tots = {n: [run_totals(d) for d in runs[n]] for n in names}
    rates = sorted(next(iter(tots.values()))[0])
    for rate in rates:
        print(f"== {rate:.0f} req/s (median over reps; per request = per completed request)")
        for n in names:
            rs = [t[rate] for t in tots[n]]
            print(f"  {n}")
            groups = sorted({g for r in rs for (g, k) in (x for x in r if isinstance(x, tuple))})
            if groups:
                print("    group        waits/req  wake/pre/slice/other   wait ms/req"
                      "  wake/pre/slice/other   >=5ms waits/s wake/pre/slice  cpu %")
            for g in groups:
                def per_req(k, i):
                    return med([r.get((g, k), [0] * 10)[i] / r["req"] for r in rs if r["req"]])

                def per_s(k, sl):
                    return med([sum(r.get((g, k), [0] * 10)[2:][sl]) / r["secs"]
                                for r in rs if r["secs"]])
                cnt = [per_req(k, 0) for k in KINDS]
                ms = [per_req(k, 1) for k in KINDS]
                long_ = [per_s(k, GE5) for k in KINDS[:3]]
                cpu = med([r.get((g, "run"), [0, 0])[1] / r["secs"] / 10 for r in rs if r["secs"]])
                print(f"    {g:11s} {sum(cnt):6.2f}  " + "/".join(f"{c:.2f}" for c in cnt)
                      + f"   {sum(ms):7.2f}  " + "/".join(f"{x:.2f}" for x in ms)
                      + "   " + "/".join(f"{x:.0f}" for x in long_) + f"   {cpu:5.1f}")
                # Runs that began by preempting a CPU (kind "preempt": count,
                # ms, then counts by run length, bounds 50/200/1000 us ...).
                prs = [(r.get((g, "preempt")), r) for r in rs if r["secs"]]
                prs = [(p, r) for p, r in prs if p and p[0]]
                if prs:
                    per_s_pre = med([p[0] / r["secs"] for p, r in prs])
                    mean_us = med([1000 * p[1] / p[0] for p, r in prs])
                    sh = [med([100 * sum(p[2:][sl]) / p[0] for p, r in prs])
                          for sl in (slice(0, 1), slice(1, 2), slice(2, 3), slice(3, None))]
                    print(f"      preempting runs {per_s_pre:.0f}/s, mean {mean_us:.0f} us; "
                          f"<50 us {sh[0]:.0f}%, 50-200 us {sh[1]:.0f}%, "
                          f"0.2-1 ms {sh[2]:.0f}%, >=1 ms {sh[3]:.0f}%")
            sl = [(r["schedlat"], r) for r in rs if r["schedlat"] and r["req"]]
            if sl:
                n_all = med([sum(c) / r["req"] for c, r in sl])
                ge5 = med([100 * sum(c[GE5]) / sum(c) for c, r in sl])
                ge20 = med([100 * sum(c[GE20]) / sum(c) for c, r in sl])
                ge5_req = med([sum(c[GE5]) / r["req"] for c, r in sl])
                print(f"    Go goroutine waits: {n_all:.1f}/req, >=5 ms {ge5:.2f}% "
                      f"({ge5_req:.3f}/req), >=20 ms {ge20:.2f}%")


if __name__ == "__main__":
    main()
