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
#   cgroups    the control groups are mounted, with Kairos's two class
#              groups (lc and batch) and the CPU controller on for both
#   sched_ext  the kernel can take a BPF scheduler
#   scheduler  Kairos Scheduler was running before init started its first
#              program
#   loader     its loader is init's child, has no terminal, and takes a
#              task's class from the batch control group
#   leftovers  init collected a finished process that it had adopted
#   terminal   init gave the program it started a terminal of its own
#   services   the service manager, init's child, runs the two demo
#              services in their groups: probe in lc, and crunch in batch
#              with all its children (one busy loop per CPU)
#   respawn    after every process of crunch was killed, the manager
#              started it again
#   restart    after its loader was killed, init started the scheduler again
#   shutdown   before the power went off, init asked a program that was
#              still running to finish
#   unload     at power off, the loader unloaded the scheduler when it was
#              asked to
#   poweroff   the system powered off by itself, without a panic
#
# With the word kairos.noscheduler, which tells init to leave the scheduler
# off, the four checks about it expect it off: scheduler (init said so and
# the kernel has none), loader and restart (no loader was ever started) and
# unload (the kernel never had a BPF scheduler to unload).
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

# Is the scheduler meant to be off?
off=0
for word in "${words[@]}"; do
	if [ "$word" = kairos.noscheduler ]; then
		off=1
	fi
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

# Is "cpu" one of the words in $1?
has_cpu() {
	case " $1 " in
	*" cpu "*) return 0 ;;
	esac
	return 1
}

cgtype=$(fact cgroups)
lc=$(fact "cgroup lc")
batch=$(fact "cgroup batch")
if [ "$cgtype" != cgroup2 ]; then
	bad cgroups "/sys/fs/cgroup is ${cgtype:-nothing}, not the control groups"
elif has_cpu "$lc" && has_cpu "$batch"; then
	ok cgroups "mounted; lc and batch have the CPU controller"
else
	bad cgroups "mounted, but lc: ${lc:-no controllers}; batch: ${batch:-no controllers}"
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
if [ $off = 1 ]; then
	if [ "$state" = disabled ] && [ "$scheduler" = none ] &&
		grep -q '^init: kairos.noscheduler: leaving Kairos Scheduler off' "$log"; then
		ok scheduler "off, as kairos.noscheduler asks; init said so"
	elif [ "$state" = disabled ] && [ "$scheduler" = none ]; then
		bad scheduler "off, but init didn't say it left it off"
	else
		bad scheduler "the scheduler was ${scheduler:-not reported} (state ${state:-not reported}) despite kairos.noscheduler"
	fi
elif [ "$state" = enabled ] && [ "$scheduler" = kairos ]; then
	ok scheduler "Kairos Scheduler ran before the first program${waited:+ (attached after $waited)}"
else
	bad scheduler "the scheduler was ${scheduler:-not reported} (state ${state:-not reported}) when the first program started"
fi

loader=$(fact loader)
if [ $off = 1 ]; then
	case $loader in
	none) ok loader "none running" ;;
	"") bad loader "not reported" ;;
	*) bad loader "running despite kairos.noscheduler: $loader" ;;
	esac
else
	rule=$(fact "loader class")
	case $loader in
	"parent 1 terminal 0")
		if [ "$rule" = "cgroup /sys/fs/cgroup/batch" ]; then
			ok loader "init's child, no terminal, batch = the batch group"
		else
			bad loader "init's child, but the class rule is ${rule:-not reported}"
		fi
		;;
	"") bad loader "not reported" ;;
	*) bad loader "$loader" ;;
	esac
fi

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

manager=$(fact manager)
probe=$(fact "service probe")
crunch=$(fact "service crunch")
if [ "$manager" != "parent 1" ]; then
	bad services "the service manager: ${manager:-not reported}"
elif [ "$probe" = "1 in lc" ] && [ "$crunch" = "$((smp + 1)) in batch" ]; then
	ok services "probe in lc; crunch in batch, with its $smp loops"
else
	bad services "probe: ${probe:-not reported}; crunch: ${crunch:-not reported} (want 1 in lc, $((smp + 1)) in batch)"
fi

respawn=$(fact respawn)
case $respawn in
*" s") ok respawn "crunch running again $respawn after all its processes were killed" ;;
*) bad respawn "crunch not running again 10 s after all its processes were killed" ;;
esac

restart=$(fact restart)
if [ $off = 1 ]; then
	# Nothing to kill, so the self-test reports none. Init must also never
	# have started the loader, at any point in the run.
	if [ "$restart" = none ] && ! grep -q '^init: starting Kairos Scheduler' "$log"; then
		ok restart "init never started the loader"
	else
		bad restart "init started the loader despite kairos.noscheduler"
	fi
else
	case $restart in
	*" s parent 1") ok restart "running again ${restart% parent 1} after its loader was killed" ;;
	none | "") bad restart "not running again 20 s after its loader was killed" ;;
	*) bad restart "$restart" ;;
	esac
fi

if [ "$(fact shutdown)" = asked ]; then
	ok shutdown "init asked a running program to finish first"
else
	bad shutdown "the power went off under a running program"
fi

# The kernel's own line when a loader closes its hold on the scheduler, and
# the loader's when it then ends. Both come before the power goes off.
# The kernel prints a line starting "sched_ext: BPF scheduler" whenever one
# is enabled or disabled; with the scheduler off there must be none.
if [ $off = 1 ]; then
	if grep -q 'sched_ext: BPF scheduler' "$log"; then
		bad unload "the kernel had a BPF scheduler despite kairos.noscheduler"
	else
		ok unload "nothing to unload: the kernel never had a BPF scheduler"
	fi
elif sed '/^reboot: /q' "$log" | grep -q 'disabled (unregistered from user space)' &&
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
