#!/usr/bin/env bash
# Boot the Kairos kernel in QEMU.
#
# usage: run-qemu.sh [--smp N] [--mem SIZE] [--kernel FILE] [--qemu ARG]...
#                    [--dtb FILE | --dump-dtb FILE] [--log FILE]
#                    [--timeout SECONDS] [--disk FILE | --no-disk]
#                    [--throwaway] [WORD...]
#
#   --smp N            number of CPUs (default 4). QEMU's form for a layout
#                      works too: 8,clusters=2,cores=2,threads=2 is 2
#                      clusters of 2 cores of 2 hardware threads each
#   --mem SIZE         memory (default 1G)
#   --kernel FILE      boot this kernel image instead of the one
#                      kernel-build.sh built; the initramfs stays the same
#   --qemu ARG         one more argument for QEMU, as it is (repeatable),
#                      e.g. memory nodes: --qemu -numa --qemu node,...
#   --dump-dtb FILE    write the device tree QEMU would give the kernel with
#                      these options to FILE, and stop
#   --dtb FILE         give the kernel this device tree instead, made with
#                      --dump-dtb and the same options, then edited (see
#                      scripts/layout-test.sh). QEMU still fills in the
#                      command line and the initramfs
#   --log FILE         run unattended: write the console's output to FILE
#                      instead of the terminal
#   --timeout SECONDS  with --log, stop QEMU after this long (default 120)
#   --disk FILE        the disk to attach (default ~/kairos-build/disk.img,
#                      if it exists; scripts/disk-build.sh builds it).
#                      Kairos sees it as /dev/vda, and root=/dev/vda on the
#                      command line tells init to make it the root
#   --no-disk          no disk: Kairos runs from the initramfs in memory
#   --throwaway        writes to the disk go to a temporary file and are lost
#                      when QEMU ends (QEMU's snapshot mode); the disk stays
#                      as it was. The tests boot this way
#   WORD...            extra words for the kernel's command line
#
# QEMU plays two parts. It is the machine: an ARM board called "virt", with
# CPUs, memory and one serial port. And it is the bootloader: it copies the
# kernel image and the initramfs archive into the machine's memory and starts
# the kernel with a command line.
#
# Without --log the console is this terminal. Leave with "poweroff" in the
# shell, or with Ctrl-A and then X.
#
# QEMU has no hardware virtualisation to use inside this VM, so it emulates
# every instruction. A boot takes seconds.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

smp=4
mem=1G
log=
timeout=120
dtb=
dump_dtb=
disk=
nodisk=0
throwaway=0
extra=()
words=()
while [ $# -gt 0 ]; do
	case $1 in
	--smp | --mem | --kernel | --qemu | --dtb | --dump-dtb | --log | --timeout | --disk)
		[ $# -ge 2 ] || die "$1 needs a value"
		case $1 in
		--smp) smp=$2 ;;
		--mem) mem=$2 ;;
		--kernel) KERNEL_IMAGE=$2 ;;
		--qemu) extra+=("$2") ;;
		--dtb) dtb=$2 ;;
		--dump-dtb) dump_dtb=$2 ;;
		--log) log=$2 ;;
		--timeout) timeout=$2 ;;
		--disk) disk=$2 ;;
		esac
		shift 2
		;;
	--no-disk)
		nodisk=1
		shift
		;;
	--throwaway)
		throwaway=1
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

[ -f "$KERNEL_IMAGE" ] || die "no kernel image at $KERNEL_IMAGE; run scripts/kernel-build.sh"

# console=ttyAMA0: the kernel's messages, and the first program's input and
# output, go to the board's serial port.
cmdline="console=ttyAMA0"

machine=virt,gic-version=3
if [ -n "$dump_dtb" ]; then
	machine=$machine,dumpdtb=$dump_dtb
fi
args=(
	# gic-version=3: the newer interrupt controller. The older default
	# stops at 8 CPUs.
	-machine "$machine"
	-cpu max
	-smp "$smp"
	-m "$mem"
	-nic none	# no network card
	-no-reboot	# when the kernel asks for a reset, end QEMU instead
	-kernel "$KERNEL_IMAGE"
)
if [ -f "$INITRAMFS" ]; then
	args+=(-initrd "$INITRAMFS")
fi
if [ -n "$dtb" ]; then
	args+=(-dtb "$dtb")
fi
args+=("${extra[@]}")

# The disk. On this board QEMU's virtio devices sit in memory-mapped slots
# that the device tree lists; virtio-blk-device puts a disk in the first
# free one, and the kernel's virtio disk driver names it /dev/vda.
if [ $nodisk = 0 ]; then
	if [ -n "$disk" ] && [ ! -f "$disk" ]; then
		die "no disk at $disk"
	fi
	disk=${disk:-$DISK}
	if [ -f "$disk" ]; then
		args+=(-drive "file=$disk,format=raw,if=none,id=disk0"
			-device virtio-blk-device,drive=disk0)
		if [ $throwaway = 1 ]; then
			args+=(-snapshot)
		fi
		cmdline="$cmdline root=/dev/vda"
	fi
fi

if [ -n "$dump_dtb" ]; then
	qemu-system-aarch64 "${args[@]}" -display none -append "$cmdline ${words[*]}"
	exit
fi

if [ -z "$log" ]; then
	exec qemu-system-aarch64 "${args[@]}" -nographic \
		-append "$cmdline ${words[*]}"
fi

# Unattended. panic=-1 makes the kernel ask for a reset as soon as it
# panics; with -no-reboot that ends QEMU, so a failed boot doesn't sit there
# until the timeout.
cmdline="$cmdline panic=-1"
start=$SECONDS
status=0
timeout --foreground "$timeout" qemu-system-aarch64 "${args[@]}" \
	-display none -monitor none -serial "file:$log" \
	-append "$cmdline ${words[*]}" || status=$?
took=$((SECONDS - start))
case $status in
0) echo "QEMU ended by itself after $took s; console output: $(wc -l <"$log") lines in $log" ;;
124) echo "QEMU was still running after $timeout s and was stopped; console output: $(wc -l <"$log") lines in $log" ;;
*) die "QEMU failed with status $status" ;;
esac
[ $status = 0 ]
