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
#   procs_before/after   every process's /proc/<pid>/stat around the load, to
#                        see what else used CPU (e.g. the VS Code server)
#
# CPU pinning: the server and batch jobs run on SUT_CPUS and the load
# generator on LOADGEN_CPUS (default 0-2 and 3), so the load generator doesn't
# compete with the work it measures. It is still scheduled by the scheduler
# under test, but only against its own threads and stray system tasks. Set
# either to empty for no pinning.
#
# SCHEDULERS is a space-separated list of entries "[label=]scheduler":
#   eevdf                    no sched_ext scheduler loaded (the kernel's default)
#   kairos[:opt1,opt2,...]   our scheduler with these loader options
# e.g. "eevdf never=kairos:--never-preempt always=kairos:--force-preempt".
# The label names the run directory (<label>-r<rep>); without one, the
# scheduler string is used with ':' and ',' replaced by '_'. For kairos runs
# the loader's per-second stats go to sched_stats.csv.
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
SUT_CPUS=${SUT_CPUS-0-2} # no colon: an explicitly empty value means unpinned
LOADGEN_CPUS=${LOADGEN_CPUS-3}
# Go would set GOMAXPROCS=1 on one CPU; then the sender thread, asleep in
# clock_nanosleep, holds the only P and replies wait (send lag p99 38 ms vs
# 4 ms with 4, EEVDF + 4 zstd, 2026-10-29).
LOADGEN_GOMAXPROCS=${LOADGEN_GOMAXPROCS:-4}
# The server's Go runtime logs one line per GC to server.log, so latency
# episodes can be checked against GC (analysis/episodes.py).
SERVER_GODEBUG=${SERVER_GODEBUG-gctrace=1}

BIN=$ROOT/workloads/bin
# Command prefixes for pinning. taskset execs the command, so $! after
# `"${SUT_PIN[@]}" cmd &` is still cmd's PID.
SUT_PIN=()
[ -n "$SUT_CPUS" ] && SUT_PIN=(taskset -c "$SUT_CPUS")
LOADGEN_PIN=()
[ -n "$LOADGEN_CPUS" ] && LOADGEN_PIN=(taskset -c "$LOADGEN_CPUS")
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
		opts=${opts//,/ }
		# $opts is unquoted on purpose: it may hold several options.
		sudo "$ROOT/scheduler/scx_kairos" $opts --stats "$2/sched_stats.csv" \
			> "$2/sched.log" 2>&1 &
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
		"${SUT_PIN[@]}" "$ROOT/workloads/batch/zstd_loop.sh" "$BATCH_INPUT" \
			"$1/batch_$i.csv" "$ZSTD_LEVEL" &
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

# snapshot <run-dir> <before|after>: system-wide and per-process CPU time.
# (Processes can exit while cat reads /proc, hence the || true.)
snapshot() {
	cp /proc/stat "$1/proc_stat_$2"
	cat /proc/[0-9]*/stat > "$1/procs_$2" 2> /dev/null || true
}

# run_one <[label=]scheduler> <rep>
run_one() {
	local entry=$1 rep=$2 sched=$1 label
	# "label=..." only if the part before the first '=' has no ':' (so an
	# option like kairos:--lat-slice-us=1000 is not mistaken for a label).
	if [[ $entry == *=* && ${entry%%=*} != *:* ]]; then
		label=${entry%%=*}
		sched=${entry#*=}
	else
		label=${sched//[:,]/_}
	fi
	local dir=$OUT/$label-r$rep
	mkdir -p "$dir"
	log "run: $sched, rep $rep -> ${dir#"$ROOT"/}"

	start_sched "$sched" "$dir"
	"$ROOT/scripts/meta.sh" "$dir" "$entry" "$WORKLOAD" > /dev/null

	GODEBUG=$SERVER_GODEBUG "${SUT_PIN[@]}" "$BIN/grpcserver" -addr "$ADDR" -workers "$WORKERS" -work-us "$WORK_US" \
		-iters-per-us "$ITERS_PER_US" > "$dir/server.log" 2>&1 &
	SERVER_PID=$!
	start_batch "$dir"
	sleep "$BATCH_WARMUP_S" # let the batch jobs reach a steady state

	snapshot "$dir" before
	GOMAXPROCS=$LOADGEN_GOMAXPROCS "${LOADGEN_PIN[@]}" "$BIN/loadgen" -addr "$ADDR" \
		-phases "$PHASES" -work-us "$WORK_US" -conns "$CONNS" -seed "$SEED" \
		-out "$dir/requests.csv" -summary "$dir/loadgen.json" 2> "$dir/loadgen.log"
	snapshot "$dir" after

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
	ITERS_PER_US=$("${SUT_PIN[@]}" "$BIN/grpcserver" -calibrate)
fi
WORKLOAD="workers=$WORKERS work_us=$WORK_US iters_per_us=$ITERS_PER_US phases=$PHASES"
WORKLOAD+=" conns=$CONNS seed=$SEED batch_jobs=$BATCH_JOBS"
WORKLOAD+=" batch_input=$(basename "$BATCH_INPUT") zstd_level=$ZSTD_LEVEL"
WORKLOAD+=" batch_warmup_s=$BATCH_WARMUP_S"
WORKLOAD+=" sut_cpus=${SUT_CPUS:-all} loadgen_cpus=${LOADGEN_CPUS:-all}"
WORKLOAD+=" loadgen_gomaxprocs=$LOADGEN_GOMAXPROCS server_godebug=$SERVER_GODEBUG"

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
