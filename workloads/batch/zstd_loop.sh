#!/usr/bin/env bash
# Batch job: compress the same input with single-threaded zstd, over and
# over, until sent SIGTERM or SIGINT. Writes one CSV line per completed
# pass, so throughput can be computed for any time window (e.g. per load
# phase). The pass that is cut short by the signal is not logged.
#
# Usage: zstd_loop.sh <input> <out.csv> [level]
set -euo pipefail

IN=${1:?input file}
OUT=${2:?output csv}
LEVEL=${3:-3}
BYTES=$(stat -c %s "$IN")

# zstd runs in the background and we `wait` for it: unlike a foreground
# command, `wait` returns as soon as a trapped signal arrives, so the trap
# can stop the current zstd too instead of leaving it running.
child=
trap '[ -n "$child" ] && kill "$child" 2>/dev/null; exit 0' TERM INT

echo "start_unix_us,end_unix_us,bytes" > "$OUT"
while :; do
	# EPOCHREALTIME is bash's wall clock in seconds with µs
	# ("1793290067.099550"); deleting the separator gives µs since the
	# epoch without starting a `date` process.
	start=${EPOCHREALTIME/[.,]/}
	zstd -q -T1 -"$LEVEL" -c "$IN" > /dev/null &
	child=$!
	wait "$child"
	end=${EPOCHREALTIME/[.,]/}
	echo "$start,$end,$BYTES" >> "$OUT"
done
