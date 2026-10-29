#!/usr/bin/env python3
"""Find latency episodes in one run and check them against the server's GC.

An episode is a run of consecutive 100 ms windows (by intended send time)
that contain a request slower than the threshold. For each episode, list the
server GC cycles (from GODEBUG=gctrace=1 lines in server.log) that were
active between 100 ms before it started and its end. For comparison, print
how many of all windows overlap a GC cycle: if most windows do, an episode
overlapping a GC says little; long GC cycles only inside episodes say more.

Usage: analysis/episodes.py <run-dir> [threshold_ms (default 50)]
"""
import csv
import json
import os
import re
import sys

WIN_NS = 100_000_000


def gc_cycles(run_dir):
    """[(start_unix_ns, end_unix_ns, wall_ms)] from server.log, or None."""
    start, out = None, []
    with open(os.path.join(run_dir, "server.log")) as f:
        for line in f:
            m = re.search(r"start_unix_ns=(\d+)", line)
            if m:
                start = int(m.group(1))
            # gc 12 @3.456s 2%: 0.012+1.2+0.003 ms clock, ...
            m = re.match(r"gc \d+ @([\d.]+)s .*?: ([\d.]+)\+([\d.]+)\+([\d.]+) ms clock", line)
            if m and start is not None:
                t = start + int(float(m.group(1)) * 1e9)
                wall = sum(float(m.group(i)) for i in (2, 3, 4))
                out.append((t, t + int(wall * 1e6), wall))
    return out if start is not None else None


def main():
    run_dir = sys.argv[1]
    thr_ns = float(sys.argv[2] if len(sys.argv) > 2 else 50) * 1e6
    with open(os.path.join(run_dir, "loadgen.json")) as f:
        t0 = json.load(f)["start_unix_ns"]
    slow, last = {}, 0  # window -> max latency (ms) among slow requests
    with open(os.path.join(run_dir, "requests.csv")) as f:
        for r in csv.DictReader(f):
            t = t0 + int(r["intended_ns"])
            last = max(last, t)
            if r["code"] == "0" and int(r["latency_ns"]) > thr_ns:
                w = t // WIN_NS
                slow[w] = max(slow.get(w, 0), int(r["latency_ns"]) / 1e6)
    gcs = gc_cycles(run_dir)
    if gcs is None:
        sys.exit("no start_unix_ns in server.log (run made before GC logging)")
    first_w, last_w = t0 // WIN_NS, last // WIN_NS
    gcs = [g for g in gcs if g[1] >= first_w * WIN_NS and g[0] <= (last_w + 1) * WIN_NS]

    def overlapping(a_ns, b_ns):
        return [g for g in gcs if g[0] < b_ns and g[1] > a_ns]

    nwin = last_w - first_w + 1
    with_gc = sum(1 for w in range(first_w, last_w + 1)
                  if overlapping(w * WIN_NS, (w + 1) * WIN_NS))
    walls = sorted(g[2] for g in gcs)
    print(f"{len(gcs)} GC cycles during the load; wall ms median "
          f"{walls[len(walls) // 2] if walls else 0:.1f}, max {walls[-1] if walls else 0:.1f}; "
          f"{with_gc}/{nwin} windows overlap a GC")

    # Group consecutive slow windows into episodes.
    eps, cur = [], []
    for w in sorted(slow):
        if cur and w != cur[-1] + 1:
            eps.append(cur)
            cur = []
        cur.append(w)
    if cur:
        eps.append(cur)
    print(f"{len(eps)} episodes with requests > {thr_ns / 1e6:.0f} ms:")
    for ep in eps:
        a, b = ep[0] * WIN_NS, (ep[-1] + 1) * WIN_NS
        g = overlapping(a - WIN_NS, b)
        gs = ", ".join(f"{x[2]:.1f}" for x in g) or "none"
        print(f"  t={(a - t0) / 1e9:5.1f}s {len(ep) * 100:5d} ms, max latency "
              f"{max(slow[w] for w in ep):6.0f} ms; GC wall ms: {gs}")


if __name__ == "__main__":
    main()
