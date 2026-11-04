#!/usr/bin/env bash
# E14 in a row, for one session with the machine otherwise idle (10 reps
# each): --claim-borrower on and off for kairos and pw1000r100,
#   claim-burst  burst-length sweep (E3's phases), 4 configs  ~2.0 h
#   claim-azure  Azure Functions 2021 replay (E2a's), 4       ~1.4 h
# Page-cache copies of earlier results and source trees are dropped before
# each part, as in final.sh. A part that fails doesn't stop the other.
# Usage: experiments/e14_claim.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

free_mem() {
	scripts/evict_cache.py "$ROOT/results" "$HOME/linux-src" "$HOME/scx" 2> /dev/null
	echo "[$(date +%T)] $1: $(free -m | awk '/^Mem:/ {print $7}') MB available" >&2
}

free_mem claim-burst
./experiments/run.sh experiments/configs/claim-burst.conf || echo "claim-burst failed" >&2
free_mem claim-azure
./experiments/run.sh experiments/configs/claim-azure.conf || echo "claim-azure failed" >&2
echo "[$(date +%T)] all done" >&2
