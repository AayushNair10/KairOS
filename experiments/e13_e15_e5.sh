#!/usr/bin/env bash
# E13 (with the guard check), E15 (pool sizing, burst sweep then Azure
# replay) and E5 (classification) in a row, in that priority order, for
# one session with the machine otherwise idle (10 reps each):
#   e13-dep          6 configs  ~2.9 h
#   e15-pool-burst   2          ~1.0 h
#   e15-pool-azure   2          ~0.7 h
#   e5-class         2          ~1.0 h
# Estimates from F: 2.8 min per burst-sweep run, 2.0 per Azure run, with
# cooldowns. With DEADLINE (a time `date -d` understands, e.g. 09:30), a
# part that wouldn't end by then is skipped (estimate + 10% + 3 min).
# Before each part the page-cache copies of earlier results and of the big
# source trees are dropped (scripts/evict_cache.py, no root), as in
# final.sh. A part that fails doesn't stop the others.
# Usage: [DEADLINE=09:30] experiments/e13_e15_e5.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
deadline=${DEADLINE:+$(date -d "$DEADLINE" +%s)}

# part <config name> <estimated minutes>
part() {
	local name=$1 est=$2 end
	end=$(( $(date +%s) + (est * 110 / 100 + 3) * 60 ))
	if [ -n "$deadline" ] && [ "$end" -gt "$deadline" ]; then
		echo "[$(date +%T)] $name: skipped, would end about $(date -d @"$end" +%T)" >&2
		return
	fi
	scripts/evict_cache.py "$ROOT/results" "$HOME/linux-src" "$HOME/scx" 2> /dev/null
	echo "[$(date +%T)] $name: $(free -m | awk '/^Mem:/ {print $7}') MB available" >&2
	./experiments/run.sh "experiments/configs/$name.conf" || echo "$name failed" >&2
}

part e13-dep 168
part e15-pool-burst 56
part e15-pool-azure 40
part e5-class 56
echo "[$(date +%T)] all done" >&2
