#!/usr/bin/env bash
# Memory pressure: does the LC side keep its memory while batch wants more
# than the machine has?
#
# usage: memory-test.sh [--mem SIZE] [--kernel FILE] [WORD...]
#
#   --mem SIZE     the machine's memory (default 192M): small, so that batch
#                  runs it short. (At 256M the LC file was pushed out in
#                  only some runs without Kairos's memory hooks: 3 of 9.)
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#                  built (see run-qemu.sh)
#   WORD...        extra words for the kernel's command line
#
# The kernel is started with kairos.test and kairos.memory, on a throwaway
# copy of the disk. For 30 s an LC program reads its 16 MB working set every
# 2 s, idle in between, while batch reads a file bigger than the machine's
# memory over and over; batch's file is all holes, so only LC reading back
# what was taken from it reads the disk (see image/selftest).
#
# The check:
#   lc-reads  the LC program's file stayed in memory: fewer than 409 of its
#             pages (a tenth of the file) were taken and read back, as the
#             memory controller counts them for the LC program's own group
#             (memory.stat's workingset_refault_file). On a kernel without
#             the memory controller: less than 1600 KiB read from the disk.
#             (Total disk reads also count other programs' pages, such as
#             the shared busybox program, charged to the root group, which
#             nothing protects; on a small machine they are read back too.)
#
# The console's output is kept in ~/kairos-build/memory-test.log. It exits
# with 0 only if the check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

mem=192M
kernel=()
words=()
while [ $# -gt 0 ]; do
	case $1 in
	--mem)
		[ $# -ge 2 ] || die "--mem needs a size"
		mem=$2
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

log=$KAIROS_BUILD/memory-test.log
rm -f "$log"
"$REPO_ROOT/scripts/run-qemu.sh" --mem "$mem" "${kernel[@]}" --throwaway --log "$log" \
	--timeout 180 kairos.test kairos.memory "${words[@]}" >/dev/null || true

line=$(sed -n 's|^kairos\.test: memory lc-reads ||p' "$log" | tr -d '\r' | head -n 1)
kib=${line%% *}
case $kib in
'' | *[!0-9]*)
	printf '%-9s FAIL  not reported (console output: %s)\n' lc-reads "$log"
	exit 1
	;;
esac
# From the LC program's own memory group, with the memory controller.
more() {
	sed -n "s|^kairos\.test: memory $1 ||p" "$log" | tr -d '\r' | head -n 1
}
refaults=$(more lc-refaults)
detail="$refaults pages read back, $line from the disk; stalled $(more lc-stall) us, protection breached $(more lc-breaches) times"
case $refaults in
'' | *[!0-9]*)
	if [ "$kib" -lt 1600 ]; then
		printf '%-9s ok    %s: the LC file stayed in memory\n' lc-reads "$line"
	else
		printf '%-9s FAIL  %s: read back from the disk\n' lc-reads "$line"
		exit 1
	fi
	;;
*)
	if [ "$refaults" -lt 409 ]; then
		printf '%-9s ok    the LC file stayed in memory: %s\n' lc-reads "$detail"
	else
		printf '%-9s FAIL  the LC file was taken: %s\n' lc-reads "$detail"
		exit 1
	fi
	;;
esac
