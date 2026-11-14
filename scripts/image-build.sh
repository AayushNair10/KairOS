#!/usr/bin/env bash
# Pack the initramfs archive: the files of Kairos's first file system, which
# the kernel unpacks into memory at boot.
#
# usage: image-build.sh
#
# image/initramfs.list names every file. The archive is written to
# ~/kairos-build/initramfs.cpio (or under $KAIROS_BUILD). Our init is
# compiled first, from userspace/init/init.c.
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

# busybox is one program that acts as many tools, depending on the name it
# is called by. Add a link in /bin for each of those names to the list. The
# full list that gets packed is kept next to the archive.
list=$KAIROS_BUILD/initramfs.list
{
	cat "$REPO_ROOT/image/initramfs.list"
	echo
	echo "# One link per tool in busybox, added by scripts/image-build.sh."
	"$BUSYBOX" --list | while IFS= read -r name; do
		[ "$name" = busybox ] || echo "slink /bin/$name busybox 0777 0 0"
	done
} >"$list"

# Our init. Like busybox it is linked statically: it carries its own copy of
# the C library. (The kernel's build has a folder called init in the build
# folder, so our programs get a folder of their own.)
INIT=$KAIROS_BUILD/userspace/init
mkdir -p "$KAIROS_BUILD/userspace"
cc -static -O2 -Wall -Wextra -Werror -o "$INIT" "$REPO_ROOT/userspace/init/init.c"

# The list refers to these three.
export BUSYBOX INIT KAIROS_REPO=$REPO_ROOT

# -t 0: give every file the same fixed date, so the same input always makes
# the same archive.
"$tool" -t 0 "$list" >"$INITRAMFS.tmp"
mv "$INITRAMFS.tmp" "$INITRAMFS"

echo "init       $INIT ($(du -h "$INIT" | cut -f1))"
echo "initramfs  $INITRAMFS ($(du -h "$INITRAMFS" | cut -f1)), busybox $("$BUSYBOX" 2>&1 | sed -n '1s/^BusyBox \(v[^ ]*\).*/\1/p') with $(grep -c '^slink /bin/' "$list") tools"
