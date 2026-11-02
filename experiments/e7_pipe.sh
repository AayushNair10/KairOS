#!/usr/bin/env bash
# E7c, context-switch cost under each scheduler: `perf bench sched pipe`
# (two tasks pass a token over a pipe; one op = one round trip = two
# wakeups and two context switches). Pinned to CPU 0, every switch goes
# through the scheduler on one CPU; on CPUs 0,1 the wakeups also cross
# CPUs. Schedulers interleaved per rep, as in run.sh.
# Usage: experiments/e7_pipe.sh [REPS]  -> results/<date>-e7-pipe/pipe.csv
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPS=${1:-5}
LOOPS=${LOOPS:-200000}
STATE=/sys/kernel/sched_ext/state
OUT=$ROOT/results/$(date +%F)-e7-pipe
[ -e "$OUT" ] && OUT=$OUT-$(date +%H%M%S)
mkdir -p "$OUT"

# name -> loader command (empty: Linux's own scheduler, EEVDF)
declare -A CMD=(
	[eevdf]=""
	[simple]="$HOME/linux-src/tools/sched_ext/build/bin/scx_simple"
	[bpfland]="$HOME/scx/target/release/scx_bpfland"
	[lavd]="$HOME/scx/target/release/scx_lavd --autopilot"
	[never3]="$ROOT/scheduler/scx_kairos --never-preempt --batch-slice-us 3000"
	[kairos]="$ROOT/scheduler/scx_kairos --split --split-cpus 0-2 --class policy --lc-buffer-pct 150"
)
ORDER="eevdf simple bpfland lavd never3 kairos"

wait_state() {
	for _ in $(seq 100); do
		[ "$(cat "$STATE")" = "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

make -C "$ROOT/scheduler" > /dev/null
"$ROOT/scripts/meta.sh" "$OUT" "$ORDER" "perf bench sched pipe -l $LOOPS, taskset -c 0 and -c 0,1, reps $REPS" > /dev/null
echo "sched,rep,cpus,usecs_per_op" > "$OUT/pipe.csv"
for rep in $(seq "$REPS"); do
	for name in $ORDER; do
		pid=
		if [ -n "${CMD[$name]}" ]; then
			# Word splitting of the command on purpose: binary + options.
			sudo ${CMD[$name]} > "$OUT/$name-r$rep.sched.log" 2>&1 &
			pid=$!
			wait_state enabled || { echo "$name did not load" >&2; exit 1; }
			sleep 1
		fi
		for cpus in 0 0,1; do
			us=$(taskset -c "$cpus" perf bench sched pipe -l "$LOOPS" 2>&1 |
				awk '/usecs\/op/ {print $1}')
			echo "$name,$rep,\"$cpus\",$us" >> "$OUT/pipe.csv"
			echo "$name r$rep cpus $cpus: $us us/op"
		done
		if [ -n "$pid" ]; then
			comm=$(basename "${CMD[$name]%% *}")
			sudo pkill -INT -x "$comm" || true
			wait "$pid" || true
			wait_state disabled || { echo "sched_ext still loaded" >&2; exit 1; }
		fi
		sleep 2
	done
done
echo "done: $OUT"
