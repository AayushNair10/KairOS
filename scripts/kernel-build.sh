#!/usr/bin/env bash
# Configure and build the Kairos kernel.
#
# usage: kernel-build.sh [--config-only] [--debug]
#
#   --config-only   put the config together and check it, but don't compile
#   --debug         add the switches in kernel/config/debug.config, which
#                   check our changes' locking, and build into
#                   ~/kairos-build-debug (or $KAIROS_BUILD-debug)
#
# The config comes from two files in kernel/config/:
#   base           the starting point: one of the configs that ship with Linux
#   kairos.config  our switches on top of it
# The kernel's config system then works out what those switches depend on.
# If a switch needs something that is off, it leaves the switch off and says
# nothing. So this script checks that every line of kairos.config made it
# into the final config, and stops if one didn't.
#
# Everything is written to ~/kairos-build (or $KAIROS_BUILD). The kernel
# image is arch/arm64/boot/Image in there. A second run compiles only what
# changed.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
need_tree

config_only=0
fragments=("$CONFIG_DIR/kairos.config")
while [ $# -gt 0 ]; do
	case $1 in
	--config-only)
		config_only=1
		shift
		;;
	--debug)
		fragments+=("$CONFIG_DIR/debug.config")
		KAIROS_BUILD=$KAIROS_BUILD-debug
		KERNEL_IMAGE=$KAIROS_BUILD/arch/arm64/boot/Image
		shift
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

# The scripts build for the machine they run on, and the image path and the
# QEMU board are arm64's.
[ "$(uname -m)" = aarch64 ] || die "only arm64 hosts are supported for now"

base=$(grep -v -e '^#' -e '^$' "$CONFIG_DIR/base" | head -n 1)
[ -n "$base" ] || die "$CONFIG_DIR/base names no starting point"
config=$KAIROS_BUILD/.config

# make, run in the kernel tree, with all output going to the build folder.
kmake() {
	make -s -C "$KAIROS_LINUX" O="$KAIROS_BUILD" "$@"
}

mkdir -p "$KAIROS_BUILD"

# 1. The starting point.
kmake "$base"
# 2. Our switches on top. -m only writes the merged file.
(cd "$KAIROS_LINUX" &&
	./scripts/kconfig/merge_config.sh -m -O "$KAIROS_BUILD" "$config" "${fragments[@]}" >/dev/null)
# 3. Let the config system settle everything the switches depend on.
kmake olddefconfig

# 4. Did every line of ours survive?
lost=0
while IFS= read -r line; do
	case $line in
	CONFIG_*=*)
		if ! grep -qxF -- "$line" "$config"; then
			echo "not in the final config:  $line" >&2
			lost=1
		fi
		;;
	"# CONFIG_"*" is not set")
		name=${line#\# }
		name=${name% is not set}
		if grep -q "^$name=" "$config"; then
			echo "still on in the final config:  $name" >&2
			lost=1
		fi
		;;
	esac
done < <(cat "${fragments[@]}")
[ $lost = 0 ] ||
	die "the config system dropped the lines above; something they depend on is off"

ours=$(cat "${fragments[@]}" | grep -c '^CONFIG_' || true)
echo "config   $base + $ours switch(es) of ours = $(grep -c '=y$' "$config") switches on"

[ $config_only = 0 ] || exit 0

start=$SECONDS
kmake -j"$(nproc)" Image
echo "build    $((SECONDS - start)) s"
echo "image    $KERNEL_IMAGE ($(du -h "$KERNEL_IMAGE" | cut -f1))"
echo "folder   $KAIROS_BUILD ($(du -sh "$KAIROS_BUILD" | cut -f1))"
