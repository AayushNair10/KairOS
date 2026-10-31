#!/usr/bin/env python3
"""Print a load-generator PHASES string: alternating quiet and busy phases.

Kinds (durations rounded to 50 ms, the ML sampling period):
  random    phases 0.25-3 s, quiet 300-700 req/s, busy 2500-3500 req/s
  short     busy 0.25-0.75 s at 3000-3500 req/s, quiet 0.5-2 s at 300-700
  periodic  E2's pattern: 5 s at 500, 5 s at 3500, repeated
The training patterns (random) and test patterns (short, periodic) differ,
so a model is never tested on the kind of pattern it learned.

Usage: ml/patterns.py KIND SEED TOTAL_S
"""
import random
import sys


def phases(kind, seed, total_s):
    rng = random.Random(seed)
    out, t, busy = [], 0.0, False
    while t < total_s:
        if kind == "periodic":
            dur, rate = 5.0, (3500 if busy else 500)
        elif kind == "random":
            dur = rng.uniform(0.25, 3.0)
            rate = rng.uniform(2500, 3500) if busy else rng.uniform(300, 700)
        elif kind == "short":
            dur = rng.uniform(0.25, 0.75) if busy else rng.uniform(0.5, 2.0)
            rate = rng.uniform(3000, 3500) if busy else rng.uniform(300, 700)
        else:
            raise SystemExit(f"unknown kind {kind}")
        dur = max(0.05, round(dur / 0.05) * 0.05)
        out.append(f"{int(round(dur * 1000))}ms@{int(rate)}")
        t += dur
        busy = not busy
    return ",".join(out)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    print(phases(sys.argv[1], int(sys.argv[2]), float(sys.argv[3])))
