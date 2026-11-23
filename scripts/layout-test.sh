#!/usr/bin/env bash
# Boot Kairos on several CPU layouts and check where Kairos Scheduler puts
# its LC pool.
#
# usage: layout-test.sh [LAYOUT...]     (default: all of them)
#
# Each layout is a machine QEMU can present: hardware threads and clusters
# (its -smp form), memory nodes (-numa), and CPUs of unequal speed, which
# QEMU can't do by itself: this script writes out QEMU's device tree for
# the layout, gives some CPUs a smaller capacity-dmips-mhz (the number the
# kernel rates a CPU's speed by), and boots with the edited tree.
#
# The kernel is started with kairos.test and kairos.layout: the self-test
# reports the CPUs' speeds and memory nodes and where the demo service
# probe ran, then ends (see image/selftest). The checks, per layout:
#   order   the order the loader put the CPUs in, which it prints when it
#           starts ("scx_kairos: split order ..."); the LC pool is the
#           first CPUs of it
#   probe   probe, nearly the only LC work, ran on the order's first two
#           CPUs, the smallest pool, in at least 90 of 100 samples (short
#           LC work can grow the pool for a moment)
#   speeds  the kernel rates the CPUs as the layout says (tooling check)
#   nodes   the kernel knows the layout's memory nodes
#
# Only placement can be checked here. Emulated hardware threads don't share
# a real core, so whether the order makes LC work faster can't be measured
# in QEMU.
#
# Console logs go to ~/kairos-build/layout-<name>.log. It exits with 0 only
# if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# name | -smp | more QEMU arguments | capacity-dmips-mhz per CPU range |
# the order | the CPUs probe may run on | the speeds | memory nodes
layouts=(
	"equal4|4|||0 1 2 3|0 1|1024 1024 1024 1024|1"
	"smt8|8,clusters=2,cores=2,threads=2|||0 1 2 3 4 5 6 7|0 1|1024 1024 1024 1024 1024 1024 1024 1024|1"
	"fast-upper8|8,clusters=2,cores=4||0-3:512,4-7:1024|4 5 6 7 0 1 2 3|4 5|512 512 512 512 1024 1024 1024 1024|1"
	# 4 clusters of 2 CPUs; node 0 holds clusters 0 and 2, node 1 the
	# others, so a node's CPUs aren't numbered together. (A cluster split
	# between nodes makes QEMU warn that Linux may misbehave.)
	"numa8|8,clusters=4,cores=2|-object memory-backend-ram,id=m0,size=512M -object memory-backend-ram,id=m1,size=512M -numa node,nodeid=0,memdev=m0,cpus=0-1,cpus=4-5 -numa node,nodeid=1,memdev=m1,cpus=2-3,cpus=6-7||0 1 4 5 2 3 6 7|0 1|1024 1024 1024 1024 1024 1024 1024 1024|2"
)

failed=0
ok() {
	printf '%-12s %-7s ok    %s\n' "$name" "$1" "$2"
}
bad() {
	printf '%-12s %-7s FAIL  %s\n' "$name" "$1" "$2"
	failed=1
}

# What the self-test printed after "kairos.test: $1 ".
fact() {
	sed -n "s|^kairos\.test: $1 ||p" "$log" | tr -d '\r' | head -n 1 | sed 's/ *$//'
}

# Of the samples in $1 (CPU:count words), how many are on the CPUs in $2.
on_cpus() {
	local w n=0
	for w in $1; do
		case " $2 " in
		*" ${w%%:*} "*) n=$((n + ${w##*:})) ;;
		esac
	done
	echo $n
}

for entry in "${layouts[@]}"; do
	IFS='|' read -r name smp more speeds_set want_order want_probe want_speeds want_nodes <<<"$entry"
	if [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$name"; then
		continue
	fi
	log=$KAIROS_BUILD/layout-$name.log
	read -ra qemu <<<"$more"
	opts=(--smp "$smp")
	for a in "${qemu[@]}"; do
		opts+=(--qemu "$a")
	done

	# Unequal speeds: QEMU's own device tree for this layout, with a
	# capacity-dmips-mhz on every CPU (the kernel ignores the numbers
	# unless every CPU has one).
	if [ -n "$speeds_set" ]; then
		dtb=$KAIROS_BUILD/layout-$name.dtb
		"$REPO_ROOT/scripts/run-qemu.sh" "${opts[@]}" --dump-dtb "$dtb" >/dev/null 2>&1
		[ -f "$dtb" ] || die "QEMU wrote no device tree for $name"
		IFS=',' read -ra ranges <<<"$speeds_set"
		for r in "${ranges[@]}"; do
			cpus=${r%%:*} value=${r##*:}
			for ((cpu = ${cpus%%-*}; cpu <= ${cpus##*-}; cpu++)); do
				fdtput -t i "$dtb" "/cpus/cpu@$cpu" capacity-dmips-mhz "$value"
			done
		done
		opts+=(--dtb "$dtb")
	fi

	rm -f "$log"
	"$REPO_ROOT/scripts/run-qemu.sh" "${opts[@]}" --log "$log" --timeout 120 \
		kairos.test kairos.layout >/dev/null || true

	order=$(sed -n 's/^scx_kairos: split order //p' "$log" | tr -d '\r' | head -n 1)
	if [ "$order" = "$want_order" ]; then
		ok order "$order"
	else
		bad order "${order:-not reported} (want $want_order)"
	fi

	ran=$(fact "probe ran on")
	if [ -n "$ran" ] && [ "$(on_cpus "$ran" "$want_probe")" -ge 90 ]; then
		ok probe "ran on CPU:samples $ran"
	else
		bad probe "ran on CPU:samples ${ran:-nothing reported} (want 90 of 100 on $want_probe)"
	fi

	speeds=$(fact speeds)
	if [ "$speeds" = "$want_speeds" ]; then
		ok speeds "$speeds"
	else
		bad speeds "${speeds:-not reported} (want $want_speeds)"
	fi

	nodes=$(fact nodes)
	# A kernel without NUMA support has no node folders: one node.
	[ "$nodes" = 0 ] && nodes=1
	if [ "$nodes" = "$want_nodes" ]; then
		ok nodes "$nodes"
	else
		bad nodes "${nodes:-not reported} (want $want_nodes)"
	fi
done

exit $failed
