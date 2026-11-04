#!/usr/bin/env bash
# The final evaluation in a row, for one long session with the machine
# otherwise idle (8 configs x 10 reps each):
#   final-burst  burst-length sweep (E3's phases)        ~3.7 h
#   final-azure  Azure Functions 2021 replay (E2a's)     ~2.7 h
# Before each part the page-cache copies of earlier results and of the big
# source trees are dropped (scripts/evict_cache.py, no root), as in
# e10_g.sh. A part that fails doesn't stop the other.
# Usage: experiments/final.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

free_mem() {
	scripts/evict_cache.py "$ROOT/results" "$HOME/linux-src" "$HOME/scx" 2> /dev/null
	echo "[$(date +%T)] $1: $(free -m | awk '/^Mem:/ {print $7}') MB available" >&2
}

free_mem final-burst
./experiments/run.sh experiments/configs/final-burst.conf || echo "final-burst failed" >&2
free_mem final-azure
./experiments/run.sh experiments/configs/final-azure.conf || echo "final-azure failed" >&2
echo "[$(date +%T)] all done" >&2
