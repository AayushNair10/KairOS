#!/usr/bin/env bash
# Boot one disk four times and check that it keeps what it is given, and
# that its journal brings it back after a power cut.
#
# usage: disk-test.sh [--kernel FILE]
#
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#                  built (see run-qemu.sh), e.g. the lock-checking build's
#
# The test works on a copy of the disk image (disk-build.sh), so the image
# itself stays fresh. Each boot has kairos.test and kairos.disk on its
# command line: the self-test reports what it finds on the disk, leaves a
# marker (the boot's number in the boot log), writes it out and ends (see
# image/selftest). The boots:
#   1  a clean boot and power-off
#   2  the same: it must find boot 1's marker, and the disk clean
#   3  with kairos.powercut: after its marker is written out, it writes one
#      more file without writing it out and cuts the power (the kernel
#      powers off at once; nothing is written out, nothing unmounted)
#   4  it must find boot 3's marker, and ext4 must replay its journal
#
# The checks:
#   root      boot 1's root is ext4 on the disk, writable
#   persist   boot 2 finds boot 1's marker, and the boot log has 2 lines
#   clean     boot 2's mount needed no recovery: boot 1 powered off
#             cleanly, so its journal was closed
#   cut       boot 3 cut the power: no clean power-off from init
#   recovery  boot 4's mount replayed the journal ("recovery complete")
#   survived  boot 4 finds boot 3's marker, written out before the cut,
#             and the boot log has 4 lines
# and, only reported: what became of the file boot 3 never wrote out.
#
# Console logs go to ~/kairos-build/disk-test-<boot>.log. It exits with 0
# only if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

kernel=()
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
	*)
		die "unknown argument: $1"
		;;
	esac
done

image=$KAIROS_BUILD/disk.img
copy=$KAIROS_BUILD/disk-test.img
[ -f "$image" ] || die "no disk image at $image: run scripts/disk-build.sh"
cp --sparse=always "$image" "$copy"

failed=0
ok() {
	printf '%-9s ok    %s\n' "$1" "$2"
}
bad() {
	printf '%-9s FAIL  %s\n' "$1" "$2"
	failed=1
}

# boot N [WORD...]: boot the copy once.
boot() {
	local n=$1
	shift
	rm -f "$KAIROS_BUILD/disk-test-$n.log"
	"$REPO_ROOT/scripts/run-qemu.sh" "${kernel[@]}" --disk "$copy" --log "$KAIROS_BUILD/disk-test-$n.log" \
		--timeout 120 kairos.test kairos.disk "$@" >/dev/null || true
}

# fact N NAME: what boot N's self-test printed after "kairos.test: NAME ".
fact() {
	sed -n "s|^kairos\.test: $2 ||p" "$KAIROS_BUILD/disk-test-$1.log" 2>/dev/null |
		tr -d '\r' | head -n 1
}

# said N TEXT: did boot N's console show TEXT?
said() {
	grep -q -- "$2" "$KAIROS_BUILD/disk-test-$1.log" 2>/dev/null
}

boot 1
boot 2
boot 3 kairos.powercut
boot 4

root=$(fact 1 root)
if [ "$root" = "ext4 /dev/vda rw" ]; then
	ok root "$root"
else
	bad root "${root:-not reported} (want ext4 /dev/vda rw)"
fi

if [ "$(fact 2 marker)" = 1 ] && [ "$(fact 2 bootlog)" = 2 ]; then
	ok persist "boot 2 found boot 1's marker; 2 lines in the boot log"
else
	bad persist "boot 2: marker $(fact 2 marker), boot log $(fact 2 bootlog) (want 1 and 2)"
fi

if [ -n "$(fact 2 root)" ] && ! said 2 'recovery complete'; then
	ok clean "boot 2 mounted the disk without a recovery"
else
	bad clean "boot 2 $(said 2 'recovery complete' && echo "replayed the journal" || echo "didn't boot")"
fi

if [ "$(fact 3 'power')" = cut ] && ! said 3 '^init: power off'; then
	ok cut "boot 3 cut the power"
else
	bad cut "boot 3 didn't cut the power"
fi

if said 4 'EXT4-fs (vda): recovery complete'; then
	ok recovery "boot 4 replayed the journal"
else
	bad recovery "boot 4 didn't replay the journal"
fi

if [ "$(fact 4 marker)" = 3 ] && [ "$(fact 4 bootlog)" = 4 ]; then
	ok survived "boot 4 found boot 3's marker; 4 lines in the boot log"
else
	bad survived "boot 4: marker $(fact 4 marker), boot log $(fact 4 bootlog) (want 3 and 4)"
fi

echo "(not judged) the file boot 3 never wrote out: $(fact 4 unsynced)"

exit $failed
