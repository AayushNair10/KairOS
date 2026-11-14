# Kairos

Kairos is an operating system built on the Linux kernel, around
[Kairos Scheduler](https://github.com/AayushNair10/KairOS-Scheduler): a CPU
scheduler for a latency-critical service that shares its CPUs with batch
work.

It has three parts:

- **The kernel:** upstream Linux at a pinned version, plus a short series of
  our own patches (`kernel/`).
- **User space:** our own init (`userspace/init/`). The shell and tools are
  busybox's for now.
- **The scheduler:** a BPF program on Linux's sched_ext, kept in its own
  repository and pinned here (`scheduler/`).

## Status

Early. A minimal Kairos kernel builds and boots in QEMU, and Kairos's own
init runs as its first process. The kernel can take a BPF scheduler, but the
scheduler isn't loaded yet. There are no kernel patches yet, and the shell
and tools are busybox's for now. The
roadmap, and the reasoning behind each decision, are in
[`docs/DESIGN_LOG.md`](docs/DESIGN_LOG.md).

## Layout

```
kernel/upstream    the pinned Linux version: its tag and commit id
kernel/patches/    our kernel changes, one numbered patch file each
kernel/config/     the kernel's config: a starting point and our switches
userspace/init/    Kairos's init: the first program the kernel starts
image/             the first file system: its list of files, the boot test's
                   self-test, and a fallback first program
scheduler/         Kairos Scheduler, a git submodule at a pinned commit
scripts/           the scripts below
docs/              DESIGN_LOG.md: the roadmap and one entry per component
```

## Build and boot

This needs an arm64 Linux machine. On Ubuntu 26.04 these packages were
enough: `git gcc make flex bison bc perl pahole libelf-dev zlib1g-dev
qemu-system-arm busybox-static`.

```sh
./scripts/kernel-fetch.sh    # once: Linux at the pinned version + our patches
./scripts/kernel-build.sh    # put the config together and build the kernel
./scripts/image-build.sh     # compile init and pack the first file system
./scripts/boot-test.sh       # boot unattended and check what the console says
./scripts/run-qemu.sh        # boot to a shell; "poweroff" leaves
```

Everything built goes to `~/kairos-build` (set `KAIROS_BUILD` for another
place). The first build takes a few minutes on 4 CPUs; a boot takes about a
second, even with QEMU emulating every instruction.

## The kernel tree

The Linux source isn't stored in this repository. `kernel/upstream` names
the version, `kernel/patches/` holds our changes to it, and
`kernel-fetch.sh` puts the two together into a working tree outside the
repository (`~/kairos-linux`; set `KAIROS_LINUX` for another place). It
downloads about 280 MB; `--from DIR` takes Linux from a git checkout that
is already on the machine instead.

To change the kernel: commit the change in the working tree, then

```sh
./scripts/kernel-export.sh   # write the tree's commits into kernel/patches/
./scripts/kernel-check.sh    # prove the tree and the patches agree; style check
```

and commit the new patch file here.

## The kernel config

`kernel/config/base` names a starting point that ships with Linux: every
optional switch off. `kernel/config/kairos.config` holds our switches on top
of it, each group with what it is for and what happened without it.
`kernel-build.sh` stops if one of our switches doesn't make it into the
final config.

## Init

`userspace/init/init.c` is the first program the kernel starts, and the
parent of everything else. It:

- mounts the file systems through which the kernel shows its own
  information;
- starts a shell, on the terminal behind the console, and starts a new one
  when that shell ends;
- collects every process that finishes, including the ones it adopted
  because their parent finished first;
- on `poweroff`, `reboot` or `halt`, asks every process to finish, waits,
  stops by force what is left, and only then tells the kernel.

`boot-test.sh` checks each of these on a running system. If init is ever
broken, `./scripts/run-qemu.sh rdinit=/init.fallback` boots with a shell
script in its place.
