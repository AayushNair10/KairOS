#!/usr/bin/env bash
# The improvement-direction experiments (RESEARCH_LOG section 9) in a row,
# for one long session with the machine otherwise idle, ~3.5 h:
#   E10       which wakeups preempt (--diag)                    ~17 min
#   E11       delayed borrower preemption                       ~97 min
#   E12       ablations: sync wakeups, per-CPU LC queues,
#             borrower choice                                   ~50 min
#   e12-pipe  perf bench sched pipe with --wake-sync             ~5 min
#   G         Redis instead of the gRPC server                  ~45 min
# Before each part the page-cache copies of earlier results and of the big
# source trees are dropped (scripts/evict_cache.py, no root), as before
# E3-r, so the batch doesn't run short of memory (RESEARCH_LOG 6.18).
# A part that fails doesn't stop the others.
# Usage: experiments/e10_g.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

free_mem() {
	scripts/evict_cache.py "$ROOT/results" "$HOME/linux-src" "$HOME/scx" 2> /dev/null
	echo "[$(date +%T)] $1: $(free -m | awk '/^Mem:/ {print $7}') MB available" >&2
}

free_mem e10
./experiments/run.sh experiments/configs/e10-preempt-diag.conf || echo "e10 failed" >&2
free_mem e11
./experiments/run.sh experiments/configs/e11-delay.conf || echo "e11 failed" >&2
free_mem e12
./experiments/run.sh experiments/configs/e12-ablate.conf || echo "e12 failed" >&2
free_mem e12-pipe
ORDER="simple never3 kairos kairossync" NAME=e12-pipe ./experiments/e7_pipe.sh 5 ||
	echo "e12-pipe failed" >&2
free_mem g
./experiments/run.sh experiments/configs/g-redis.conf || echo "g failed" >&2
echo "[$(date +%T)] all done" >&2
