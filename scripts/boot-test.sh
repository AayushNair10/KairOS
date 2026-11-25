#!/usr/bin/env bash
# Boot Kairos unattended and check what the console says.
#
# usage: boot-test.sh [--smp N] [--kernel FILE] [--no-disk] [WORD...]
#
#   --smp N    number of CPUs to give the machine (default 4), or a layout
#              in QEMU's form, such as 8,clusters=2,cores=2,threads=2
#   --kernel FILE  boot this kernel image instead of the one kernel-build.sh
#              built (see run-qemu.sh)
#   --no-disk  boot without the disk: Kairos stays in the initramfs, in
#              memory, and the root checks expect that
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
#   root       the root file system is ext4 on the disk (/dev/vda), and
#              writable (with --no-disk: the initramfs, in memory)
#   initramfs  after the switch to the disk, the initramfs is no longer
#              mounted (with --no-disk: it still is)
#   bootlog    init added this boot's line to the boot log on the disk; the
#              test boots a throwaway copy of a fresh disk, so it is the
#              first line (with --no-disk: there is no boot log)
#   devices    the kernel filled in /dev
#   cgroups    the control groups are mounted, with Kairos's two class
#              groups (lc and batch) and the CPU controller on for both
#   memory     the memory controller is on for lc and batch too
#   io         the I/O controller is on for lc and batch too
#   iosched    the disk's I/O scheduler is mq-deadline, which serves
#              requests by priority class, and a lower class waits at most
#              2000 ms (with --no-disk: there is no disk)
#   sched_ext  the kernel can take a BPF scheduler
#   scheduler  Kairos Scheduler was running before init started its first
#              program
#   early      the kernel attached it before it started init: the kernel
#              said so, and sched_ext's "enabled" line comes before "Run
#              /init"
#   loader     its loader is init's child, has no terminal, and adopted
#              the scheduler attached before init instead of loading one
#   leftovers  init collected a finished process that it had adopted
#   terminal   init gave the program it started a terminal of its own
#   services   the service manager, init's child, runs the two demo
#              services in their groups: probe in lc, and crunch (started
#              with "service start") in batch with all its children (one
#              busy loop per CPU); "service status" shows probe running
#   kclass     the kernel's class for each group (its kairos controller):
#              lc and batch hold their classes, and the groups below them
#              take theirs, including crunch's after it moved to lc
#   respawn    after every process of crunch was killed, the manager
#              started it again
#   stop       "service stop" stopped a service whose only process was
#              left behind by its first and ignores the request to finish
#   batch      "batch COMMAND" ran the command in a group below batch, and
#              removed the group afterwards
#   relabel    one write made crunch's group LC, and a group made below it
#              started as LC; "inherit" gave both batch's class back; a
#              word that isn't a class was refused
#   oomscore   the kernel would kill batch first when memory runs out:
#              crunch's out-of-memory score, as batch, is at least 300
#              above the probe's, and after one write makes it LC, it isn't
#              (with kairos.nomemclass, which turns that off: never)
#   class      with every CPU busy with crunch, a small LC program's median
#              wake-up delay was at most half as long with crunch as batch
#              as with crunch as LC (one write to its group, while it
#              runs). Only reported, not judged, with
#              kairos.noscheduler, or when the machine has more CPUs than
#              the computer running QEMU: each emulated CPU is a thread
#              there, and the delays measure how those threads share the
#              real CPUs
#   restart    after its loader was killed, the scheduler kept running (no
#              scheduler was enabled again), and init started a new loader
#   shutdown   before the power went off, init asked a program that was
#              still running to finish
#   unload     at power off, the loader unloaded the scheduler when it was
#              asked to
#   poweroff   the system powered off by itself, without a panic
#
# With the word kairos.noscheduler, which tells the kernel and init to leave
# the scheduler off, the five checks about it expect it off: scheduler (init
# said so and the kernel has none), early (the kernel said so and attached
# nothing), loader and restart (no loader was ever started) and unload (the
# kernel never had a BPF scheduler to unload).
#
# It exits with 0 only if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

smp=4
kernel=()
# The disk (if disk-build.sh made one), as a throwaway copy: every boot
# test starts from a fresh disk, and the disk itself stays as it is.
disk=(--throwaway)
nodisk=0
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
	--no-disk)
		disk=(--no-disk)
		nodisk=1
		shift
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

# The number of CPUs: --smp's value, or the first part of a layout. (Never
# the whole layout in arithmetic: bash reads "8,clusters=2,..." as a list of
# assignments.)
ncpu=${smp%%,*}
case $ncpu in
'' | *[!0-9]*) die "--smp $smp: no number of CPUs at its start" ;;
esac

# Is the scheduler meant to be off? Are the memory hooks?
off=0
memclass=1
for word in "${words[@]}"; do
	if [ "$word" = kairos.noscheduler ]; then
		off=1
	fi
	if [ "$word" = kairos.nomemclass ]; then
		memclass=0
	fi
done

log=$KAIROS_BUILD/boot-test.log
rm -f "$log"
# The limit: a test with 16 CPUs took 88 s on a 4-CPU machine, most of it
# emulating crunch's 16 busy loops during the class measurement.
"$REPO_ROOT/scripts/run-qemu.sh" --smp "$smp" "${kernel[@]}" "${disk[@]}" --log "$log" --timeout 180 \
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
# its lines with a carriage return, which is cut off. Names may hold a "/"
# (a group's path), so the pattern is set off by "|".
fact() {
	sed -n "s|^kairos\.test: $1 ||p" "$log" | tr -d '\r' | head -n 1
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
if [ "$cpus" = "$ncpu" ]; then
	ok cpus "$cpus of $ncpu running"
else
	bad cpus "${cpus:-none} of $ncpu running"
fi

if [ $nodisk = 1 ]; then
	want_root="rootfs rootfs rw" want_initramfs=mounted want_bootlog=none
else
	want_root="ext4 /dev/vda rw" want_initramfs=gone want_bootlog=1
fi
root=$(fact root)
if [ "$root" = "$want_root" ]; then
	ok root "$root"
else
	bad root "${root:-not reported} (want $want_root)"
fi
initramfs=$(fact initramfs)
if [ "$initramfs" = "$want_initramfs" ]; then
	ok initramfs "$initramfs"
else
	bad initramfs "${initramfs:-not reported} (want $want_initramfs)"
fi
bootlog=$(fact bootlog)
if [ "$bootlog" = none ] && [ $nodisk = 1 ]; then
	ok bootlog "none: no disk"
elif [ "$bootlog" = "$want_bootlog" ]; then
	ok bootlog "$bootlog line(s)"
else
	bad bootlog "${bootlog:-not reported} (want $want_bootlog)"
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

# Is $1 one of the words in $2?
has_word() {
	case " $2 " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}
if has_word memory "$lc" && has_word memory "$batch"; then
	ok memory "on for lc and batch"
else
	bad memory "lc: ${lc:-no controllers}; batch: ${batch:-no controllers} (want memory in both)"
fi
if has_word io "$lc" && has_word io "$batch"; then
	ok io "on for lc and batch"
else
	bad io "lc: ${lc:-no controllers}; batch: ${batch:-no controllers} (want io in both)"
fi
iosched=$(fact iosched)
if [ $nodisk = 1 ]; then
	if [ "$iosched" = "no disk" ]; then
		ok iosched "no disk"
	else
		bad iosched "${iosched:-not reported} (want no disk)"
	fi
else
	case $iosched in
	*"[mq-deadline]"*" aging 2000") ok iosched "$iosched" ;;
	*) bad iosched "${iosched:-not reported} (want [mq-deadline], aging 2000)" ;;
	esac
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

# The console's line numbers of the first "enabled" line and of the kernel
# starting /init (0: not there). Only the part before the self-test's first
# line counts: the boot itself.
line_of() {
	sed '/^kairos\.test: /q' "$log" | tr -d '\r' | grep -n -m 1 -- "$1" | cut -d: -f1 || true
}
enabled_at=$(line_of 'sched_ext: BPF scheduler "kairos" enabled')
init_at=$(line_of '^Run /init as init process')
if [ $off = 1 ]; then
	if [ -z "$enabled_at" ] &&
		grep -q '^kairos: kairos.noscheduler: not attaching Kairos Scheduler' "$log"; then
		ok early "nothing attached before init, as kairos.noscheduler asks; the kernel said so"
	elif [ -z "$enabled_at" ]; then
		bad early "nothing attached, but the kernel didn't say it left the scheduler off"
	else
		bad early "a scheduler was enabled during the boot despite kairos.noscheduler"
	fi
elif [ -z "$init_at" ]; then
	bad early "the kernel never started /init"
elif [ -n "$enabled_at" ] && [ "$enabled_at" -lt "$init_at" ] &&
	grep -q '^kairos: Kairos Scheduler attached before init' "$log"; then
	ok early "the kernel attached Kairos Scheduler before it started init"
elif [ -n "$enabled_at" ] && [ "$enabled_at" -lt "$init_at" ]; then
	bad early "enabled before init, but the kernel didn't say it attached it"
else
	bad early "not attached before init (enabled ${enabled_at:+at console line $enabled_at, }/init at line $init_at)"
fi

loader=$(fact loader)
if [ $off = 1 ]; then
	case $loader in
	none) ok loader "none running" ;;
	"") bad loader "not reported" ;;
	*) bad loader "running despite kairos.noscheduler: $loader" ;;
	esac
else
	mode=$(fact "loader mode")
	case $loader in
	"parent 1 terminal 0")
		if [ "$mode" = adopt ]; then
			ok loader "init's child, no terminal, adopted the scheduler attached before init"
		elif [ "$mode" = load ]; then
			bad loader "init's child, but it loaded the scheduler itself"
		else
			bad loader "init's child, but the self-test didn't say how it got the scheduler"
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
status=$(fact "status probe")
if [ "$manager" != "parent 1" ]; then
	bad services "the service manager: ${manager:-not reported}"
elif [ "$probe" = "1 in lc" ] && [ "$crunch" = "$((ncpu + 1)) in batch" ]; then
	if [ "$status" = "lc running" ]; then
		ok services "probe in lc; crunch in batch, with its $ncpu loops; status agrees"
	else
		bad services "running, but service status says probe is \"${status}\""
	fi
else
	bad services "probe: ${probe:-not reported}; crunch: ${crunch:-not reported} (want 1 in lc, $((ncpu + 1)) in batch)"
fi

kclass=("lc $(fact "kclass lc")" "batch $(fact "kclass batch")"
	"batch/crunch $(fact "kclass batch/crunch")")
moved=$(fact "kclass lc/crunch")
want=("lc inherit lc" "batch batch batch" "batch/crunch inherit batch")
if [ "${kclass[*]}" != "${want[*]}" ]; then
	bad kclass "$(IFS=';'; echo "${kclass[*]}") (want $(IFS=';'; echo "${want[*]}"))"
elif [ "${moved% *}" != "inherit lc" ] || [ "${moved##* }" -lt 1 ]; then
	bad kclass "crunch moved to lc: ${moved:-not reported} (want inherit lc, and its processes)"
else
	ok kclass "batch set, lc inherits LC; crunch inherits batch, and lc after it moved"
fi

respawn=$(fact respawn)
case $respawn in
*" s") ok respawn "crunch running again $respawn after all its processes were killed" ;;
*) bad respawn "crunch not running again 10 s after all its processes were killed" ;;
esac

stop=$(fact stop)
case $stop in
"1 before, 0 after, stopped, "*) ok stop "the process left behind was gone, after ${stop##*, }" ;;
*) bad stop "${stop:-not reported} (want: 1 before, 0 after, stopped)" ;;
esac

batch=$(fact batch)
left=$(fact "batch groups left")
case $batch in
"0::/batch/cmd-"*)
	if [ "$left" = 0 ]; then
		ok batch "ran in ${batch#0::}; the group was removed after"
	else
		bad batch "ran in ${batch#0::}, but ${left:-unknown} group(s) left behind"
	fi
	;;
*) bad batch "ran in: ${batch:-not reported}" ;;
esac

relabel=$(fact relabel)
want="lc: crunch lc, below lc; inherit: crunch batch, below batch; nonsense refused"
if [ "$relabel" = "$want" ]; then
	ok relabel "one write each way, followed by the group below; nonsense refused"
else
	bad relabel "${relabel:-not reported} (want $want)"
fi

# Medians and 99th percentiles of the probe's wake-up delays, in us.
read -r idle_med idle_p99 <<<"$(fact "class idle")"
read -r batch_med batch_p99 <<<"$(fact "class batch")"
read -r lc_med lc_p99 <<<"$(fact "class lc")"
delays="median ${batch_med:-?} us as batch, ${lc_med:-?} us as LC, ${idle_med:-?} us without crunch"
if [ -z "$batch_med" ] || [ -z "$lc_med" ] || [ -z "$idle_med" ]; then
	bad class "not measured"
elif [ $off = 1 ]; then
	ok class "for reference, under EEVDF: $delays"
elif [ "$ncpu" -gt "$(nproc)" ]; then
	ok class "not judged, $ncpu emulated CPUs on $(nproc) real ones: $delays"
elif [ $((batch_med * 2)) -le "$lc_med" ]; then
	ok class "$delays"
else
	bad class "$delays: as batch isn't at most half of as LC"
fi

# Out-of-memory scores: crunch's and the probe's, with crunch batch, then
# LC.
read -r batch_score _ lc_score <<<"$(fact "oom batch")"
read -r moved_score _ moved_lc <<<"$(fact "oom relabelled")"
scores="crunch as batch $batch_score, probe $lc_score; crunch as LC $moved_score, probe $moved_lc"
case "$batch_score$lc_score$moved_score$moved_lc" in
'' | *[!0-9]*) bad oomscore "not reported: $scores" ;;
*)
	boosted=$((batch_score >= lc_score + 300))
	still=$((moved_score >= moved_lc + 300))
	if [ $memclass = 1 ] && [ $boosted = 1 ] && [ $still = 0 ]; then
		ok oomscore "$scores"
	elif [ $memclass = 0 ] && [ $boosted = 0 ] && [ $still = 0 ]; then
		ok oomscore "not by class, as kairos.nomemclass asks: $scores"
	else
		bad oomscore "$scores"
	fi
	;;
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
	*" s parent 1 reloads 0")
		ok restart "the scheduler outlived its killed loader; init started a new loader ${restart% parent 1 *} later"
		;;
	*" s parent 1 reloads "*)
		bad restart "the scheduler went with its killed loader: enabled again ${restart##* reloads } time(s)"
		;;
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
