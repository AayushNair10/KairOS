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
# facts, and shuts the system down when it ends (see image/selftest). The
# console's output is kept in ~/kairos-build/boot-test.log.
#
# The checks:
#   kernel     the kernel that booted calls itself "...-kairos"
#   init       process 1 is our init, started from the initramfs
#   cpus       all the machine's CPUs are running
#   devices    the kernel filled in /dev
#   sched_ext  the kernel can take a BPF scheduler
#   scheduler  Kairos Scheduler was running before init started its first
#              program
#   loader     its loader is init's child and has no terminal
#   leftovers  init collected a finished process that it had adopted
#   terminal   init gave the program it started a terminal of its own
#   shutdown   before the power went off, init asked a program that was
#              still running to finish
#   unload     at power off, the loader unloaded the scheduler when it was
#              asked to
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
case $state in
missing | "") bad sched_ext "the kernel can't take a BPF scheduler" ;;
*) ok sched_ext "present, $state" ;;
esac

# How long init waited for it, from init's own line on the console.
waited=$(sed -n 's/^init: Kairos Scheduler is running, after \([0-9]* ms\).*/\1/p' "$log" |
	tr -d '\r' | head -n 1)
scheduler=$(fact scheduler)
if [ "$state" = enabled ] && [ "$scheduler" = kairos ]; then
	ok scheduler "Kairos Scheduler ran before the first program${waited:+ (attached after $waited)}"
else
	bad scheduler "the scheduler was ${scheduler:-not reported} (state ${state:-not reported}) when the first program started"
fi

loader=$(fact loader)
case $loader in
"parent 1 terminal 0") ok loader "init's child, no terminal" ;;
"") bad loader "not reported" ;;
*) bad loader "$loader" ;;
esac

leftovers=$(fact leftovers)
if [ "$leftovers" = 0 ]; then
	ok leftovers "init collected the finished process it adopted"
else
	bad leftovers "${leftovers:-unknown number of} finished process(es) not collected"
fi

terminal=$(fact terminal)
case $terminal in
/dev/*) ok terminal "init gave the self-test the terminal $terminal" ;;
*) bad terminal "the self-test had no terminal" ;;
esac

if [ "$(fact shutdown)" = asked ]; then
	ok shutdown "init asked a running program to finish first"
else
	bad shutdown "the power went off under a running program"
fi

# The kernel's own line when a loader closes its hold on the scheduler, and
# the loader's when it then ends. Both come before the power goes off.
if sed '/^reboot: /q' "$log" | grep -q 'disabled (unregistered from user space)' &&
	sed '/^reboot: /q' "$log" | grep -q '^EXIT: unregistered from user space'; then
	ok unload "the loader unloaded the scheduler when asked"
else
	bad unload "the scheduler wasn't unloaded on request before the power went off"
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
