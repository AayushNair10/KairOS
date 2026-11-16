#!/usr/bin/env bash
# Build Kairos Scheduler's loader for the Kairos kernel.
#
# usage: scheduler-build.sh
#
# The loader is the ordinary program that hands the scheduling policy to the
# kernel and then holds it in place. Its sources are in scheduler/scheduler/:
# the policy (scx_kairos.bpf.c), the loader (scx_kairos.c) and a Makefile.
#
# Everything is written to ~/kairos-build/scheduler (or under $KAIROS_BUILD):
#
#   tools/   what the build needs from the kernel tree, built from the pinned
#            source: the BPF library, its command-line tool, and a header
#            that describes every data type of the Kairos kernel
#   src/     a copy of the scheduler's sources, with what gets built from
#            them. The loader is src/scx_kairos.
#
# The header is made from the kernel that scripts/kernel-build.sh built, so
# run that first.
#
# A second run builds only what changed.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
need_tree

case ${1:-} in
"") ;;
-h | --help)
	sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
	exit 0
	;;
*) die "unknown argument: $1" ;;
esac

from=$REPO_ROOT/scheduler/scheduler
[ -f "$from/Makefile" ] ||
	die "no scheduler sources in $from; run: git submodule update --init"

# The kernel with its type descriptions still in it. The image that boots is
# made from this file.
vmlinux=$KAIROS_BUILD/vmlinux
[ -f "$vmlinux" ] || die "no kernel at $vmlinux; run scripts/kernel-build.sh"

out=$KAIROS_BUILD/scheduler
tools=$out/tools/build
src=$out/src
mkdir -p "$out/tools" "$src"

# 1. The tools, with the Makefile that builds the kernel's own example
#    schedulers. Only three of its targets are asked for:
#      libbpf.a    the BPF library, which the loader is linked with
#      bpftool     packs the compiled policy into a C header for the loader
#      vmlinux.h   every data type of the kernel, as C declarations. The
#                  policy is compiled against it.
#    VMLINUX_BTF says which kernel to describe. Left out, the Makefile would
#    take the one this machine is running.
make -s -j"$(nproc)" -C "$KAIROS_LINUX/tools/sched_ext" \
	O="$out/tools" VMLINUX_BTF="$vmlinux" \
	"$tools/obj/libbpf/libbpf.a" "$tools/sbin/bpftool" \
	"$tools/include/vmlinux.h"

# 2. The sources. The scheduler's Makefile builds in the folder it is in, so
#    it runs on a copy and the submodule stays as it was checked out.
#    -p keeps each file's date, so an unchanged file isn't compiled again.
rm -f "$src"/Makefile "$src"/*.c "$src"/*.h
cp -p "$from"/Makefile "$from"/*.c "$from"/*.h "$src/"

# 3. The loader, with the scheduler's own Makefile.
#      KSRC, SCX_BUILD   our kernel tree and the tools from step 1, in place
#                        of the ones the scheduler project uses
#      LDLIBS            -static: the initramfs holds no shared libraries, so
#                        the loader carries its own copy of each library it
#                        uses. zstd is one that the ELF library needs.
make -s -C "$src" KSRC="$KAIROS_LINUX" SCX_BUILD="$tools" \
	LDLIBS="-static -lelf -lz -lzstd -lpthread"

file "$SCHEDULER" | grep -q 'statically linked' ||
	die "$SCHEDULER isn't statically linked"

echo "types      $(grep -c '' "$tools/include/vmlinux.h") lines describe the kernel in $vmlinux"
echo "loader     $SCHEDULER ($(du -h "$SCHEDULER" | cut -f1)), statically linked"
