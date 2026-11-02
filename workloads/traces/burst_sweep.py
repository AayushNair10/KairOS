#!/usr/bin/env python3
"""Print a load-generator PHASES string for the burst-length sweep (E3).

One block per burst length L: busy phases of L at --busy req/s alternate
with quiet phases of L at --quiet req/s. The duty cycle is 50% in every
block, so the mean load is the same and only the time scale changes. A
block has at least --min-bursts bursts and lasts at least --block-s. Before
each block, --gap-s at the quiet rate gives every block the same start.
Blocks are replayed in an order shuffled with --seed, so no length is always
first or always follows the same other length. The gap's duration must not
be one of the lengths: the analysis tells blocks apart by phase duration.

Usage: workloads/traces/burst_sweep.py [--lengths-ms 10,30,100,...]
       [--busy 3500] [--quiet 500] [--block-s 16] [--min-bursts 2]
       [--gap-s 2] [--seed 1]
Prints PHASES on stdout and the blocks on stderr.
"""
import argparse
import math
import random
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lengths-ms", default="10,30,100,300,1000,3000,10000")
    ap.add_argument("--busy", type=int, default=3500)
    ap.add_argument("--quiet", type=int, default=500)
    ap.add_argument("--block-s", type=float, default=16)
    ap.add_argument("--min-bursts", type=int, default=2)
    ap.add_argument("--gap-s", type=float, default=2)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    lengths = [int(x) for x in a.lengths_ms.split(",")]
    gap_ms = int(a.gap_s * 1000)
    if gap_ms in lengths:
        raise SystemExit(f"--gap-s {a.gap_s} equals a burst length")
    order = lengths[:]
    random.Random(a.seed).shuffle(order)
    phases, total = [], 0
    for L in order:
        n = max(a.min_bursts, math.ceil(a.block_s * 1000 / (2 * L)))
        phases.append(f"{gap_ms}ms@{a.quiet}")
        phases += [f"{L}ms@{a.busy}", f"{L}ms@{a.quiet}"] * n
        total += gap_ms + 2 * n * L
        print(f"{L} ms: {n} bursts, {2 * n * L / 1000:.1f} s", file=sys.stderr)
    print(f"{len(phases)} phases, {total / 1000:.1f} s", file=sys.stderr)
    print(",".join(phases))


if __name__ == "__main__":
    main()
