#!/usr/bin/env bash
# Build Kairos's disk: an ext4 file system image holding the same files as
# the initramfs, plus the folders that only make sense on a disk.
#
# usage: disk-build.sh [--size SIZE]
#
#   --size SIZE  the disk's size, as mke2fs takes it (default 256M)
#
# The image is ~/kairos-build/disk.img (or under $KAIROS_BUILD). QEMU shows
# it to Kairos as a virtio disk, /dev/vda (scripts/run-qemu.sh), and init
# makes it the root. It is sparse: only the blocks in use take room.
#
# The files come from the initramfs archive (scripts/image-build.sh, run it
# first), unpacked into a folder; so the disk and the initramfs always
# hold the same system. Two things the archive has can't be copied this
# way, and the disk doesn't need them: device entries (the kernel fills in
# /dev), and the owner: unpacked by this user, every file would belong to
# it. mke2fs copies the owner from the folder (it could take a tar archive
# with owners in it, but only with libarchive, which isn't installed), so
# afterwards debugfs, e2fsprogs' tool for editing an unmounted file
# system, gives every file to user 0. No root needed for any of it.
#
# Building it again throws away whatever Kairos wrote to the disk.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

size=256M
while [ $# -gt 0 ]; do
	case $1 in
	--size)
		[ $# -ge 2 ] || die "--size needs a value"
		size=$2
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

[ -f "$INITRAMFS" ] || die "no initramfs at $INITRAMFS; run scripts/image-build.sh"

# 1. The files: the archive, unpacked. cpio can't make device entries
# without root and says so; the rest is unpacked.
root=$KAIROS_BUILD/disk-root
rm -rf "$root"
mkdir -p "$root"
(cd "$root" && cpio -i -d -m --quiet --no-absolute-filenames <"$INITRAMFS" 2>&1 |
	grep -v 'Operation not permitted' || true)
rm -rf "$root/dev"/*

# 2. What only the disk has:
#   /dev        where init moves the kernel's device entries
#   /var/log    logs kept from one boot to the next (init's boot log)
#   /var/lib    other state kept across boots
mkdir -p "$root/dev" "$root/var/log/kairos" "$root/var/lib"

# 3. The file system. -d fills it from the folder; -L names it.
rm -f "$DISK"
mke2fs -q -t ext4 -L kairos -d "$root" "$DISK" "$size"

# 4. Every file to user 0 and group 0 ("sif": set an inode field).
(cd "$root" && find . -mindepth 1 | sed 's|^\.||' |
	while IFS= read -r path; do
		echo "sif $path uid 0"
		echo "sif $path gid 0"
	done) | debugfs -w -f - "$DISK" >/dev/null 2>&1
debugfs -w -R "sif / uid 0" "$DISK" >/dev/null 2>&1
debugfs -w -R "sif / gid 0" "$DISK" >/dev/null 2>&1

# 5. A check of the whole file system, changing nothing (-n).
e2fsck -f -n "$DISK" >/dev/null 2>&1 || die "e2fsck found errors in $DISK"

echo "disk       $DISK ($size, $(du -h "$DISK" | cut -f1) in use), $(find "$root" | wc -l) files and folders"
