#!/usr/bin/env bash
# Boot Kairos unattended and check what the console says.
#
# usage: boot-test.sh [--smp N] [WORD...]
#
#   --smp N    number of CPUs to give the machine (default 4)
#   WORD...    extra words for the kernel's command line
#
# The kernel is started with the word kairos.test on its command line. The
# first program then runs /selftest in place of the shell, which prints a few
# facts, and the system powers off (see image/selftest). The console's output
# is kept in ~/kairos-build/boot-test.log.
#
# The checks:
#   kernel     the kernel that booted calls itself "...-kairos"
#   init       process 1 is our init, started from the initramfs
#   cpus       all the machine's CPUs are running
#   devices    the kernel filled in /dev
#   sched_ext  the kernel can take a BPF scheduler, and none is loaded
#   poweroff   the system powered off by itself, without a panic
#
# It exits with 0 only if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

smp=4
words=()
while [ $# -gt 0 ]; do
	case $1 in
	--smp)
		[ $# -ge 2 ] || die "--smp needs a number"
		smp=$2
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

log=$KAIROS_BUILD/boot-test.log
rm -f "$log"
"$REPO_ROOT/scripts/run-qemu.sh" --smp "$smp" --log "$log" --timeout 120 \
	kairos.test "${words[@]}" || true
[ -f "$log" ] || die "QEMU wrote no console output"

failed=0
ok() {
	printf '%-10s ok    %s\n' "$1" "$2"
}
bad() {
	printf '%-10s FAIL  %s\n' "$1" "$2"
	failed=1
}

# The value /selftest printed after "kairos.test: <name> ". The console ends
# its lines with a carriage return, which is cut off.
fact() {
	sed -n "s/^kairos\.test: $1 //p" "$log" | tr -d '\r' | head -n 1
}

version=$(fact kernel)
case $version in
*-kairos*) ok kernel "Linux $version" ;;
"") bad kernel "the self-test didn't report a kernel version" ;;
*) bad kernel "Linux $version doesn't call itself -kairos" ;;
esac

first=$(fact init)
if [ "$first" = /init ]; then
	ok init "process 1 is our init"
elif [ -n "$first" ]; then
	bad init "process 1 is $first, not our init"
elif grep -q '^Run /init as init process' "$log"; then
	bad init "the kernel started /init, but the self-test didn't run"
else
	bad init "the kernel never started /init"
fi

cpus=$(fact cpus)
if [ "$cpus" = "$smp" ]; then
	ok cpus "$cpus of $smp running"
else
	bad cpus "${cpus:-none} of $smp running"
fi

devices=$(fact devices)
devices=${devices%% *}
if [ "${devices:-0}" -gt 1 ]; then
	ok devices "$devices entries in /dev"
else
	bad devices "${devices:-0} entries in /dev"
fi

state=$(fact sched_ext)
if [ "$state" = disabled ]; then
	ok sched_ext "present, no scheduler loaded"
else
	bad sched_ext "${state:-not reported}"
fi

if grep -q 'Kernel panic' "$log"; then
	bad poweroff "$(grep -m 1 'Kernel panic' "$log" | tr -d '\r' | cut -c1-90)"
elif grep -q '^reboot: Power down' "$log"; then
	ok poweroff "powered off by itself"
else
	bad poweroff "the system didn't power off"
fi

[ $failed = 0 ] || echo "console output: $log"
exit $failed
