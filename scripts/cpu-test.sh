#!/usr/bin/env bash
# CPU contention: how late are an LC program's wakeups while batch work
# keeps every CPU busy, and how many tasks run ahead of it?
#
# usage: cpu-test.sh [--smp N] [--kernel FILE] [WORD...]
#
#   --smp N        CPUs (default 4)
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#                  built (see run-qemu.sh)
#   WORD...        extra words for the kernel's command line;
#                  kairos.noscheduler measures Linux's own scheduler (EEVDF),
#                  and with kairos.batchidle too, with batch's group made an
#                  idle group (cpu.idle)
#
# The kernel is started with kairos.test and kairos.cpu, on a throwaway
# copy of the disk. An LC program wakes every 2 ms for 4 s: without
# crunch, with crunch (one busy loop per CPU) as batch, and with crunch
# relabelled LC. For each, the median and 99th percentile of its wakeup
# delays, and from the kernel's trace of wakeups and task switches, how
# many other tasks had its CPU before it ran (see image/selftest). Set
# KAIROS_ICOUNT to measure in instruction-counted time (see run-qemu.sh).
#
# The checks:
#   traced  the trace lost no events (otherwise the counts are partial)
#   class   (only with Kairos Scheduler) as batch, the median delay is at
#           most half of as LC
#
# The console's output is kept in ~/kairos-build/cpu-test.log.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

smp=4
kernel=()
words=()
while [ $# -gt 0 ]; do
	case $1 in
	--smp)
		[ $# -ge 2 ] || die "--smp needs a number"
		smp=$2
		shift 2
		;;
	--kernel)
		[ $# -ge 2 ] || die "--kernel needs a file"
		kernel=(--kernel "$2")
		shift 2
		;;
	-h | --help)
		sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
		exit 0
		;;
	-*)
		die "unknown option: $1"
		;;
	*)
		words+=("$1")
		shift
		;;
	esac
done
off=0
for word in "${words[@]}"; do
	[ "$word" = kairos.noscheduler ] && off=1
done

log=$KAIROS_BUILD/cpu-test.log
rm -f "$log"
"$REPO_ROOT/scripts/run-qemu.sh" --smp "$smp" "${kernel[@]}" --throwaway --log "$log" \
	--timeout 180 kairos.test kairos.cpu "${words[@]}" >/dev/null || true

fact() {
	sed -n "s|^kairos\.test: cpu $1 ||p" "$log" | tr -d '\r' | head -n 1
}
lost=0
for k in idle batch lc; do
	# median p99 ahead wakeups mean none one-or-less most lost N
	read -r med p99 _ n mean none one most _ l <<<"$(fact $k)"
	echo "(reported) $k: median ${med:-?} us, 99% ${p99:-?} us; ${n:-?} wakeups: ${mean:-?} tasks ahead on average, ${none:-?} with none, ${one:-?} with at most one, ${most:-?} at most"
	[ "${l:-1}" = 0 ] || lost=1
done
failed=0
if [ $lost = 0 ]; then
	echo "traced ok    no trace events lost"
else
	echo "traced FAIL  trace events lost, or not reported (console output: $log)"
	failed=1
fi
[ $off = 1 ] && exit $failed
read -r b_med _ <<<"$(fact batch)"
read -r l_med _ <<<"$(fact lc)"
case "$b_med$l_med" in
'' | *[!0-9]*)
	echo "class  FAIL  not reported (console output: $log)"
	exit 1
	;;
esac
if [ $((b_med * 2)) -le "$l_med" ]; then
	echo "class  ok    median delay: $b_med us with crunch as batch, $l_med us as LC"
else
	echo "class  FAIL  median delay: $b_med us with crunch as batch, $l_med us as LC"
	failed=1
fi
exit $failed
