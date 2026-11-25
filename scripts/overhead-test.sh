#!/usr/bin/env bash
# What a wakeup and a task switch cost: the probe's ping-pong.
#
# usage: overhead-test.sh [--smp N] [--kernel FILE] [WORD...]
#
#   --smp N        CPUs (default 4)
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#                  built (see run-qemu.sh)
#   WORD...        extra words for the kernel's command line;
#                  kairos.noscheduler measures Linux's own scheduler (EEVDF)
#
# The kernel is started with kairos.test and kairos.overhead, on a
# throwaway copy of the disk. Two processes pass a byte back and forth
# through pipes 20000 times: two wakeups and two task switches a round
# trip. First both on CPU 0, then wherever the scheduler puts them (see
# image/selftest). Only reported, in ns per round trip. Meant for
# instruction-counted time (KAIROS_ICOUNT, see run-qemu.sh): otherwise the
# numbers are emulated time, and depend on the host's load.
#
# The console's output is kept in ~/kairos-build/overhead-test.log.

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

log=$KAIROS_BUILD/overhead-test.log
rm -f "$log"
"$REPO_ROOT/scripts/run-qemu.sh" --smp "$smp" "${kernel[@]}" --throwaway --log "$log" \
	--timeout 180 kairos.test kairos.overhead "${words[@]}" >/dev/null || true

fact() {
	sed -n "s|^kairos\.test: overhead $1 ||p" "$log" | tr -d '\r' | head -n 1
}
one=$(fact one-cpu) any=$(fact any-cpu)
echo "(reported) a round trip (2 wakeups, 2 task switches): ${one:-?} ns on one CPU, ${any:-?} ns on any CPU"
[ -n "$one" ] && [ -n "$any" ]
