#!/usr/bin/env bash
# Pack the initramfs archive: the files of Kairos's first file system, which
# the kernel unpacks into memory at boot.
#
# usage: image-build.sh
#
# image/initramfs.list names every file. The archive is written to
# ~/kairos-build/initramfs.cpio (or under $KAIROS_BUILD).
#
# The shell and tools come from the machine's own busybox for now
# ($BUSYBOX to use another one). It has to be linked statically, because the
# archive holds no shared libraries. On Ubuntu that is the busybox-static
# package.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
need_tree

BUSYBOX=${BUSYBOX:-$(command -v busybox || true)}
[ -n "$BUSYBOX" ] && [ -x "$BUSYBOX" ] || die "busybox not found; install busybox-static or set BUSYBOX"
file -L "$BUSYBOX" | grep -q 'statically linked' ||
	die "$BUSYBOX isn't statically linked; install busybox-static or set BUSYBOX"

mkdir -p "$KAIROS_BUILD"

# The packing tool is a single C file in the kernel tree. It reads the list
# and writes the archive, and needs no root to give files to user 0 or to
# set their modes.
tool=$KAIROS_BUILD/gen_init_cpio
if [ ! -x "$tool" ] || [ "$KAIROS_LINUX/usr/gen_init_cpio.c" -nt "$tool" ]; then
	cc -O2 -o "$tool" "$KAIROS_LINUX/usr/gen_init_cpio.c"
fi

# The list refers to these two.
export BUSYBOX KAIROS_REPO=$REPO_ROOT

# -t 0: give every file the same fixed date, so the same input always makes
# the same archive.
"$tool" -t 0 "$REPO_ROOT/image/initramfs.list" >"$INITRAMFS.tmp"
mv "$INITRAMFS.tmp" "$INITRAMFS"

echo "initramfs  $INITRAMFS ($(du -h "$INITRAMFS" | cut -f1)), busybox $("$BUSYBOX" 2>&1 | sed -n '1s/^BusyBox \(v[^ ]*\).*/\1/p')"
