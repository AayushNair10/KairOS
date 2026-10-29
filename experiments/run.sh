#!/usr/bin/env bash
# Run an experiment config: for every repetition and every scheduler in the
# config, start the scheduler, the gRPC server and the batch jobs, drive the
# server with the open-loop load generator, then stop everything and keep
# the raw logs.
#
# Usage: ./experiments/run.sh experiments/configs/<name>.conf
#
# Output: results/<date>-<NAME>/config.conf, plus one directory per run,
# results/<date>-<NAME>/<scheduler>-r<rep>/, containing:
#   meta.json            git commit, kernel, scheduler, workload parameters
#   requests.csv         one row per request (from loadgen)
#   loadgen.json, .log   per-phase latency summary
#   batch_<i>.csv        one row per completed zstd pass, per batch job
#   server.log           grpcserver output
#   sched.log            scheduler loader output (sched_ext schedulers only)
#   proc_stat_before/after  /proc/stat around the load (CPU time, ctx switches)
#
# Scheduler names: "eevdf" (no sched_ext scheduler loaded: the kernel's
# default) or "kairos" / "kairos:<loader options>" (our scheduler).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONF=$(realpath "${1:?usage: run.sh <config>}")
cd "$ROOT"
source "$CONF"

# Defaults for anything the config doesn't set.
: "${NAME:?the config must set NAME}"
SCHEDULERS=${SCHEDULERS:-eevdf}
REPS=${REPS:-1}
COOLDOWN_S=${COOLDOWN_S:-10}
ADDR=${ADDR:-127.0.0.1:50051}
WORKERS=${WORKERS:-16}
WORK_US=${WORK_US:-100}
ITERS_PER_US=${ITERS_PER_US:-}  # empty: measure once, before the first run
PHASES=${PHASES:-10s@1000}
CONNS=${CONNS:-8}
SEED=${SEED:-1}
BATCH_JOBS=${BATCH_JOBS:-4}
BATCH_INPUT=${BATCH_INPUT:-$HOME/kairos-data/zstd-input-64M.tar}
ZSTD_LEVEL=${ZSTD_LEVEL:-3}
BATCH_WARMUP_S=${BATCH_WARMUP_S:-2}

BIN=$ROOT/workloads/bin
STATE=/sys/kernel/sched_ext/state
SCHED_PID=
SERVER_PID=
BATCH_PIDS=()

log() { echo "[$(date +%T)] $*" >&2; }

# alive <pid>: the process exists and hasn't exited. /proc/<pid>/stat is
# readable even for root's processes (unlike kill -0); "Z" means it has
# exited but hasn't been waited for yet.
alive() {
	local s
	s=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null) && [ "$s" != Z ]
}

# stop_pid <pid>: SIGTERM, wait up to 5 s, then SIGKILL.
stop_pid() {
	kill -TERM "$1" 2>/dev/null || return 0
	for _ in $(seq 50); do
		alive "$1" || break
		sleep 0.1
	done
	alive "$1" && kill -KILL "$1" 2>/dev/null
	wait "$1" 2>/dev/null || true
}

# wait_state <state>: wait up to 10 s for sched_ext to report <state>. When
# waiting for "enabled", give up early if the loader has already exited.
wait_state() {
	for _ in $(seq 100); do
		[ "$(cat "$STATE")" = "$1" ] && return 0
		if [ "$1" = enabled ] && ! alive "$SCHED_PID"; then
			return 1
		fi
		sleep 0.1
	done
	return 1
}

# start_sched <scheduler> <run-dir>
start_sched() {
	case $1 in
	eevdf)
		;;
	kairos | kairos:*)
		local opts=${1#kairos}
		opts=${opts#:}
		# $opts is unquoted on purpose: it may hold several options.
		sudo "$ROOT/scheduler/scx_kairos" $opts > "$2/sched.log" 2>&1 &
		SCHED_PID=$!
		if ! wait_state enabled || [ "$(cat /sys/kernel/sched_ext/root/ops)" != kairos ]; then
			log "scx_kairos did not load; see $2/sched.log"
			return 1
		fi
		;;
	*)
		log "unknown scheduler '$1'"
		return 1
		;;
	esac
}

# stop_sched: unload our scheduler by sending SIGINT to its loader, as Ctrl-C
# does. (Wrapping the loader in `sudo timeout` instead got it SIGKILLed
# before it printed its exit reason.)
stop_sched() {
	[ -n "$SCHED_PID" ] || return 0
	sudo pkill -INT -x scx_kairos || true
	wait "$SCHED_PID" || true
	SCHED_PID=
	wait_state disabled || log "warning: sched_ext is still '$(cat "$STATE")'"
}

# start_batch <run-dir>: BATCH_JOBS copies of the zstd loop, one CSV each.
start_batch() {
	for ((i = 0; i < BATCH_JOBS; i++)); do
		"$ROOT/workloads/batch/zstd_loop.sh" "$BATCH_INPUT" "$1/batch_$i.csv" "$ZSTD_LEVEL" &
		BATCH_PIDS+=($!)
	done
}

stop_batch() {
	for pid in "${BATCH_PIDS[@]}"; do
		stop_pid "$pid"
	done
	BATCH_PIDS=()
}

# Stop whatever is still running if the script exits early (error, Ctrl-C).
cleanup() {
	set +e
	stop_batch
	[ -n "$SERVER_PID" ] && stop_pid "$SERVER_PID"
	stop_sched
}
trap cleanup EXIT
trap 'log "interrupted"; exit 130' INT TERM

# run_one <scheduler> <rep>
run_one() {
	local sched=$1 rep=$2
	local dir=$OUT/${sched//:/_}-r$rep
	mkdir -p "$dir"
	log "run: $sched, rep $rep -> ${dir#"$ROOT"/}"

	start_sched "$sched" "$dir"
	"$ROOT/scripts/meta.sh" "$dir" "$sched" "$WORKLOAD" > /dev/null

	"$BIN/grpcserver" -addr "$ADDR" -workers "$WORKERS" -work-us "$WORK_US" \
		-iters-per-us "$ITERS_PER_US" > "$dir/server.log" 2>&1 &
	SERVER_PID=$!
	start_batch "$dir"
	sleep "$BATCH_WARMUP_S" # let the batch jobs reach a steady state

	cp /proc/stat "$dir/proc_stat_before"
	"$BIN/loadgen" -addr "$ADDR" -phases "$PHASES" -work-us "$WORK_US" \
		-conns "$CONNS" -seed "$SEED" -out "$dir/requests.csv" \
		-summary "$dir/loadgen.json" 2> "$dir/loadgen.log"
	cp /proc/stat "$dir/proc_stat_after"

	stop_batch
	stop_pid "$SERVER_PID"
	SERVER_PID=
	stop_sched
	python3 "$ROOT/analysis/summarize_run.py" "$dir"
}

# Build first, so the binaries match the commit recorded in meta.json.
make -C scheduler > /dev/null
make -C workloads > /dev/null
if [ ! -f "$BATCH_INPUT" ]; then
	log "missing $BATCH_INPUT; create it with workloads/batch/make_input.sh"
	exit 1
fi
if [ "$(cat "$STATE")" != disabled ]; then
	log "a sched_ext scheduler is already loaded: $(cat /sys/kernel/sched_ext/root/ops)"
	exit 1
fi

# Same work per request in every run of this batch: calibrate once, now,
# while nothing else is running.
if [ -z "$ITERS_PER_US" ]; then
	ITERS_PER_US=$("$BIN/grpcserver" -calibrate)
fi
WORKLOAD="workers=$WORKERS work_us=$WORK_US iters_per_us=$ITERS_PER_US phases=$PHASES"
WORKLOAD+=" conns=$CONNS seed=$SEED batch_jobs=$BATCH_JOBS"
WORKLOAD+=" batch_input=$(basename "$BATCH_INPUT") zstd_level=$ZSTD_LEVEL"
WORKLOAD+=" batch_warmup_s=$BATCH_WARMUP_S"

OUT=$ROOT/results/$(date +%F)-$NAME
[ -e "$OUT" ] && OUT=$OUT-$(date +%H%M%S) # never overwrite earlier raw data
mkdir -p "$OUT"
cp "$CONF" "$OUT/config.conf"

read -r -a scheds <<< "$SCHEDULERS"
first=1
for rep in $(seq 1 "$REPS"); do
	# Reverse the order on every other repetition, so a slow drift (e.g.
	# the fanless host heating up) doesn't always hit the same scheduler.
	order=("${scheds[@]}")
	if ((rep % 2 == 0)); then
		order=()
		for ((i = ${#scheds[@]} - 1; i >= 0; i--)); do
			order+=("${scheds[i]}")
		done
	fi
	for s in "${order[@]}"; do
		if [ -z "$first" ]; then
			log "cooldown ${COOLDOWN_S}s"
			sleep "$COOLDOWN_S"
		fi
		first=
		run_one "$s" "$rep"
	done
done
log "done: ${OUT#"$ROOT"/}"
