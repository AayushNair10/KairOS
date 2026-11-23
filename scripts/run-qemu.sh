#!/usr/bin/env bash
# Boot the Kairos kernel in QEMU.
#
# usage: run-qemu.sh [--smp N] [--mem SIZE] [--kernel FILE] [--log FILE]
#                    [--timeout SECONDS] [WORD...]
#
#   --smp N            number of CPUs (default 4)
#   --mem SIZE         memory (default 1G)
#   --kernel FILE      boot this kernel image instead of the one
#                      kernel-build.sh built; the initramfs stays the same
#   --log FILE         run unattended: write the console's output to FILE
#                      instead of the terminal
#   --timeout SECONDS  with --log, stop QEMU after this long (default 120)
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
words=()
while [ $# -gt 0 ]; do
	case $1 in
	--smp | --mem | --kernel | --log | --timeout)
		[ $# -ge 2 ] || die "$1 needs a value"
		case $1 in
		--smp) smp=$2 ;;
		--mem) mem=$2 ;;
		--kernel) KERNEL_IMAGE=$2 ;;
		--log) log=$2 ;;
		--timeout) timeout=$2 ;;
		esac
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

[ -f "$KERNEL_IMAGE" ] || die "no kernel image at $KERNEL_IMAGE; run scripts/kernel-build.sh"

# console=ttyAMA0: the kernel's messages, and the first program's input and
# output, go to the board's serial port.
cmdline="console=ttyAMA0"

args=(
	# gic-version=3: the newer interrupt controller. The older default
	# stops at 8 CPUs.
	-machine virt,gic-version=3
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
