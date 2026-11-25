#!/usr/bin/env bash
# Disk contention: do the LC side's disk requests go first while batch
# keeps the disk busy?
#
# usage: io-test.sh [--kernel FILE] [WORD...]
#
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#                  built (see run-qemu.sh)
#   WORD...        extra words for the kernel's command line
#
# The kernel is started with kairos.test and kairos.io, on a throwaway copy
# of the disk, slowed down: QEMU serves at most 200 requests a second, and
# virtio_blk.queue_depth=1 lets the kernel hand the disk one request at a
# time, so the others wait in the kernel, where the I/O scheduler decides
# their order (a disk with a shallow queue); the self-test gives the
# scheduler room for 64. An LC program reads 4 KiB past the page cache
# every 50 ms, first alone, then while batch reads back to back (8
# readers) and writes 128 MB; then it writes 4 KiB and waits for the disk
# (fsync) every 200 ms while batch's writes are written out (see
# image/selftest).
#
# The checks:
#   reads    the LC program's median read time with batch at work is under
#            20 ms. With the disk busy each request takes about 5 ms (200
#            a second). Behind the whole queue (a request from each of the
#            8 readers) LC waits about 40 ms; first in line, it waits for
#            the one request at the disk, then its own. 20 ms is halfway.
#            (Not a multiple of its time alone: alone, its few requests
#            never meet QEMU's limit, and take about 1 ms.)
#   charged  batch's writes are charged to batch's group by the I/O
#            controller: at least 64 MiB of the 128 MiB it wrote
#   starved  with LC keeping the disk busy, batch still gets reads done,
#            and none of them waits more than 3 s (mq-deadline serves a
#            batch request once it has waited its aging limit, 2 s)
# and, only reported: the LC program's fsync time.
#
# The console's output is kept in ~/kairos-build/io-test.log. It exits with
# 0 only if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

kernel=()
words=()
while [ $# -gt 0 ]; do
	case $1 in
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

log=$KAIROS_BUILD/io-test.log
rm -f "$log"
"$REPO_ROOT/scripts/run-qemu.sh" "${kernel[@]}" --throwaway --disk-limit 200 --log "$log" \
	--timeout 240 kairos.test kairos.io virtio_blk.queue_depth=1 "${words[@]}" >/dev/null || true

fact() {
	sed -n "s|^kairos\.test: io $1 ||p" "$log" | tr -d '\r' | head -n 1
}

failed=0
read -r alone alone99 <<<"$(fact lc-alone)"
read -r loaded loaded99 <<<"$(fact lc-loaded)"
read -r fsync fsync99 <<<"$(fact lc-fsync)"
writes=$(fact batch-writes)
times="LC reads: median ${alone:-?} us alone (99% ${alone99:-?}), ${loaded:-?} us with batch (99% ${loaded99:-?})"
case "$alone$loaded" in
'' | *[!0-9]*)
	printf '%-8s FAIL  not reported: %s (console output: %s)\n' reads "$times" "$log"
	failed=1
	;;
*)
	if [ "$loaded" -lt 20000 ]; then
		printf '%-8s ok    %s\n' reads "$times"
	else
		printf '%-8s FAIL  %s: 20 ms or more, behind batch\n' reads "$times"
		failed=1
	fi
	;;
esac
case $writes in
'' | *[!0-9]*)
	printf '%-8s FAIL  batch wrote 128 MiB; its group was charged %s\n' charged "${writes:-nothing (no io.stat)}"
	failed=1
	;;
*)
	if [ "$writes" -ge $((64 * 1048576)) ]; then
		printf '%-8s ok    batch wrote 128 MiB; its group was charged %s MiB\n' charged $((writes / 1048576))
	else
		printf '%-8s FAIL  batch wrote 128 MiB; its group was charged %s MiB\n' charged $((writes / 1048576))
		failed=1
	fi
	;;
esac
read -r breads bmost <<<"$(fact batch-starved)"
case "$breads$bmost" in
'' | *[!0-9]*)
	printf '%-8s FAIL  batch reported no reads while LC kept the disk busy\n' starved
	failed=1
	;;
*)
	if [ "$breads" -ge 1 ] && [ "$bmost" -le 3000000 ]; then
		printf '%-8s ok    batch did %s reads in 8 s with LC keeping the disk busy; the longest took %s ms\n' starved "$breads" $((bmost / 1000))
	else
		printf '%-8s FAIL  batch did %s reads in 8 s; the longest took %s ms\n' starved "$breads" $((bmost / 1000))
		failed=1
	fi
	;;
esac
echo "(reported) LC fsync with batch writing: median ${fsync:-?} us, 99% ${fsync99:-?} us"
exit $failed
