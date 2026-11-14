# Kairos design log

One entry per component, appended at its wrap-up. Changed decisions are
recorded as new entries; old entries aren't rewritten.

## Roadmap (tentative; version 1, 2026-11-11)

Kairos is option D of component 0: an upstream Linux kernel at a pinned
version plus a short series of our own patches, our own init and user space,
and the scheduling policy as a BPF program on sched_ext.

| # | Component | What it teaches | Kernel changes |
|---|---|---|---|
| 0 | What kind of OS Kairos is (done) | the boot path; scheduling classes | none |
| 1 | Source layout: how Linux and the scheduler live in the repo (done) | how kernel changes are carried: tags, patch series, forks | none |
| 2 | Kernel config and first boot in QEMU, to a busybox shell (done) | Kconfig, kbuild, device tree, initramfs, console | config only |
| 3 | Our own init (PID 1) (done) | processes: fork, exec, wait, signals, reaping; sessions and terminals; orderly shutdown; mounting /proc, /sys, /dev | none |
| 4 | Kairos Scheduler loaded and supervised by init | the bpf() syscall, struct_ops attach, BTF; what an ejection does | none |
| 5 | Shell and a service manager that sets task classes | sessions and terminals, cgroups, scheduling policies | none |
| 6 | Task class as a kernel concept (syscall or cgroup file, visible to BPF) | syscall entry, task_struct, user ABI, kfuncs | patch |
| 7 | LC and batch pools that follow CPU topology | sched domains, NUMA, CPU capacity | policy; maybe a patch |
| 8 | Scheduler attached before PID 1 | initcalls, kernel threads, loading BPF from inside the kernel | patch (not sized) |
| 9 | A root file system on a virtio disk | VFS, block layer, virtio drivers, page cache, journaling | config; maybe none |
| 10 | Memory reclaim that knows LC from batch | page allocator, reclaim, memory cgroups | patch |
| 11 | Block I/O that knows LC from batch | I/O schedulers, I/O priority | patch |
| 12 | Measurement at real speed | needs QEMU on the Mac (hvf) or a second VM; ask first | none |
| 13 | Native scheduling class: decide with measurements | the scheduler core | maybe |

- Components 1 to 5 need no kernel changes. They end with a system that
  boots into Kairos.
- Components 6 to 11 are the kernel patches. Each has to do something that
  user space or BPF can't.
- Drivers are only used in component 9, not written. A small driver of our
  own could be added.

---

## 0. What kind of OS Kairos is — 2026-11-11

Options:
- A: a minimal Linux distribution. Unmodified upstream kernel with our
  config, our own init and user space in an initramfs, the BPF scheduler
  loaded by init.
- B: a Linux fork with the policy ported to a native `struct sched_class`,
  plus a task-class attribute, a syscall and class awareness in other
  subsystems.
- C: a from-scratch arm64 teaching kernel for QEMU's `virt` board, with
  Linux as the reference.
- D: A's user space, a pinned upstream kernel plus a short series of our own
  patches, and the policy kept in BPF on sched_ext.

My pick and reasoning:
- Conflicted between B and D. Ready to go with B if nothing outside the VM
  is affected.
- A requirement added with the answer: Kairos is a general-purpose OS that
  is the best home for the scheduler. It isn't tuned to the dev VM's 4 vCPUs.
- After the verdict: D.

Verdict: D, with the native class as a late component (13) decided with
measurements, not the foundation.
- B and D differ in one thing: whether the policy runs as a native class or
  stays in BPF. B's other parts are D's patches.
- The port would put the largest block of work into scheduling, the area
  already known best, ahead of boot, memory, files and drivers.
- The general-purpose requirement makes the port bigger. The ext core does
  shared queues across run queues, CPU hotplug, cgroups and the fallback
  today; a port has to rebuild them.
- No measurement says BPF holds the scheduler back: all its callbacks used
  about 4% of one CPU under load, against 1.5% for scx_simple (scheduler
  repo, RESEARCH_LOG 6.20).
- The BPF scheduler's evaluation stays valid. A port's numbers would have to
  be measured again, and that can't be done inside the VM (no KVM).
- Right: narrowing to B and D; asking about isolation before committing.
- Wrong: isolation as the condition for B. It is the same for every option.
- Missed: the general-purpose requirement points toward D; B's numbers can't
  be measured inside the VM.

Where the others fit:
- A: systems whose value is in user space and that want no kernel
  maintenance (Alpine; Buildroot and Yocto images).
- B: a policy that needs what sched_ext can't give (kernels before 6.12, a
  policy that must also govern real-time tasks, no tolerance for a
  fallback), or a vendor that ships its own kernel for years (Qualcomm's
  WALT in Android vendor kernels, PREEMPT_RT as an out-of-tree series until
  6.12, MuQSS).
- C: when building each mechanism is the point, or Linux's size is the
  problem (xv6, seL4, Fuchsia's Zircon).

Linux concepts learned (read in `~/linux-src`, v7.0):
- Boot on arm64: `primary_entry` and `__primary_switched`
  (`arch/arm64/kernel/head.S`) → `start_kernel()` → `rest_init()` →
  `kernel_init()` → `kernel_init_freeable()` → `do_initcalls()`
  (`init/main.c`); `populate_rootfs()` (`init/initramfs.c`);
  `run_init_process("/init")`.
- PID 1 is `kernel_init`; PID 2 is `kthreadd`; the boot thread becomes CPU
  0's idle task.
- `struct sched_class` (`kernel/sched/sched.h`, about 30 callbacks). Class
  order comes from the linker script (`include/asm-generic/vmlinux.lds.h`):
  stop, dl, rt, fair, ext, idle.
- `__setscheduler_class()` (`kernel/sched/core.c`) and `task_should_scx()`
  pick a task's class. `bpf_scx_reg()` → `scx_enable()`
  (`kernel/sched/ext.c`) switches every eligible task to ext;
  `next_active_class()` then skips fair.
- What the ext core does for a BPF policy: `consume_remote_task()` and
  `move_remote_task_to_local_dsq()` (a task handed between two run-queue
  locks), `handle_hotplug()`, the cgroup hooks, `scx_bypass()`.
- `CONFIG_SCHED_CLASS_EXT` depends on `BPF_SYSCALL`, `BPF_JIT` and
  `DEBUG_INFO_BTF` (`kernel/Kconfig.preempt`).

What changed: no code. Kairos is defined as a Linux-based OS in three
parts: a pinned upstream kernel plus our patch series, our own init and
user space, and the policy as a BPF program. A kernel patch is for
something user space or BPF can't do.

How it's tested: nothing to run in this component. QEMU 10.2.1 was installed
for the later ones (`qemu-system-aarch64 --version`; `apt-get install
--no-install-recommends qemu-system-arm`, 97 MB). Nothing has been booted.

Interview questions:
1. Walk me through what happens from the moment the kernel image gets
   control until the first user process runs. Where does PID 1 come from?
2. Your scheduler is a BPF program loaded from user space. Who schedules the
   system before it's attached, and how does the kernel hand existing tasks
   over when it attaches?
3. Why didn't you write your scheduler as a native scheduling class? What
   would that have required, and what would you have gained and lost?

Resume line: a placeholder, since nothing is built yet. "Designing Kairos,
a Linux-based OS around my eBPF CPU scheduler: an upstream kernel with a
small patch series, its own init and user space."

Open questions / next:
- Component 1: how the Linux source and the scheduler repo live in this
  repo.
- The patch for attaching the scheduler before PID 1 (component 8) isn't
  sized.
- Performance can't be measured in QEMU inside the VM (no `/dev/kvm`). It
  needs QEMU on the Mac with hvf or a second VM; ask first.
- The policy picks LC CPUs in CPU order and was measured on 3 CPUs sharing
  one cache. A general-purpose OS needs pools that follow topology
  (component 7).
- The first kernel build needs `flex` and `bison`, which aren't installed.
- The VM has 12 GB of free disk. A kernel tree and its build may need 5 to
  8 GB (a guess).

---

## 1. Source layout — 2026-11-13

Options, decision 1 (where Linux's source and our changes live):
- 1A: this repo is a Linux fork, with our own files in a `kairos/` directory.
- 1B: a pinned tag plus a patch series in this repo; a script puts them
  together into a working tree outside it.
- 1C: a separate fork repo, referenced from this one as a submodule.

Options, decision 2 (how the scheduler gets in):
- 2A: a git submodule of the scheduler repo, pinned at a commit.
- 2B: a vendored copy of its four source files.
- 2C: a kernel patch that adds it to `tools/sched_ext/`.

My pick and reasoning:
- 1B: "this code is going to my resume/github repo, and I would like to
  display my changes rather than the whole thing".
- 2A: "I can have the scheduler as a single commit, since that was
  implemented by me".

Verdict: 1B and 2A, the picks.
- 1B, right: a repo that shows only our work is the strongest argument,
  and it holds because option D keeps the series short.
- 1B, incomplete: that reason only rules out 1A. Under 1C this repo would
  also hold only our files. 1C loses on overhead: a second, Linux-sized
  repo and two commits per kernel change.
- 1B, missed costs: GitHub classes patch files as data, so the language bar
  won't count kernel work; a change lives in two places (the working tree
  and the patch file) and they can drift apart.
- 2A, right pick with a wrong picture: a submodule doesn't add the
  scheduler here as a commit. It stores a pointer to one commit of the
  other repo. Adding the code itself in one commit would be 2B.
- 2A, missed: OS-driven policy changes (components 6 and 7) become commits
  in the scheduler repo. They go on a branch there, so `main` stays as
  evaluated.
- 2B loses: two copies of a scheduler still owned and maintained.
- 2C loses: under 1B it adds a 2,900-line patch that isn't a kernel change.

Where the others fit:
- 1A: teams that live in the kernel tree with large, continuous changes
  (Ubuntu's kernel, the Android common kernel, Raspberry Pi's kernel).
- 1C: many repos from many teams pinned together (Android, ChromiumOS).
- 2B: builds without a network, or code that belongs to someone else
  (SQLite's single-file source, the kernel's copies of compression
  libraries).
- 2C: tools maintained and released with the kernel (its example
  schedulers, perf).

Linux concepts learned:
- Releases: a tag per release (`v7.0`); stable fixes on a `linux-7.0.y`
  branch. `v7.0` is an annotated tag, an object of its own that points at
  the commit `028ef9c96e96`.
- A commit id is a hash of the files and of all history behind them. A tree
  hash covers the files only. Two trees with the same hash hold the same
  files.
- Patches: `git format-patch` writes a commit as a mail-shaped text file,
  `git am` turns it back into a commit, `scripts/checkpatch.pl` checks
  style, and it requires a `Signed-off-by:` line.
- Git's index: the list of every file and its content hash. `git read-tree`
  fills one, `git apply --cached` applies a patch to it without touching
  files on disk, `git write-tree` gives the tree hash.
- A shallow clone (`--depth 1`) holds one release without its history:
  280 MB instead of several GB.
- A submodule is stored as one entry of a special type (mode 160000) that
  holds a commit id, plus the URL in `.gitmodules`.
- `make O=dir` builds outside the source tree.
- `tools/sched_ext/Makefile` builds libbpf and bpftool and generates
  `vmlinux.h` from a kernel's BTF; it looks at `$(O)/vmlinux` first and at
  the running kernel's `/sys/kernel/btf/vmlinux` after that. The scheduler's
  Makefile reuses all three through `KSRC`.

What changed: this repo holds only our own work. `kernel/upstream` pins
Linux 7.0 by tag and by commit id, and `kernel/patches/` will hold our
kernel changes as a patch series, which is the source of truth. Three
scripts tie it together: one rebuilds the working tree `~/kairos-linux`
from the pin and the patches, one writes the tree's commits out as patch
files, and one proves that the two agree and runs the kernel's style
checker. The scheduler is a pointer to one commit of its own repo. There
are no kernel patches yet.

How it's tested:
- `./scripts/kernel-fetch.sh --from ~/linux-src`, from nothing: 1 min, a
  2.0 GB tree on branch `kairos`, tree hash `6e2e13adb3eb`. A second run
  refuses to overwrite. A changed pin is rejected before any file is
  written.
- `./scripts/kernel-check.sh`: five lines (pin, clean, rebuild, series,
  style), all `ok`, exit status 0.
- Round trip with a throwaway one-line change to `init/main.c`: export,
  delete the tree, fetch again. Same tree hash (`40d461ca00c6`) and the same
  commit id (`9e584e43b981`) before and after; a second export gives the
  same file. The change was removed again.
- The check fails when it should: a patch file edited by hand (rebuild and
  series), an uncommitted change (clean), a commit that wasn't exported
  (rebuild and series), a commit with trailing whitespace and no sign-off
  (style).
- A fresh clone with `--recurse-submodules` has `scheduler/` at `5a38f5f`,
  passes the check, and builds `scx_kairos` with
  `make -C scheduler/scheduler KSRC=~/linux-src`; the submodule stays clean
  and `~/linux-src` is unchanged.

Interview questions:
1. Your repo doesn't contain the kernel source. How does someone get from
   it to exactly the kernel you built, and how do you know the patches in
   the repo match the tree you worked in?
2. Why pin a dependency by commit id and not only by its tag or version
   name? What does the id cover?
3. What does your repo store for the scheduler, and how is that different
   from copying the scheduler's code in?

Resume line: "Built the source workflow for Kairos's kernel: upstream Linux
pinned by commit id, changes kept as a patch series, and a check that
rebuilds the tree from the patches and compares content hashes."

Open questions / next:
- Component 2: kernel config and first boot in QEMU. The build needs `flex`
  and `bison`, which aren't installed; ask first.
- Disk: 9.3 GB free after the 2 GB tree. The first kernel build may need
  several GB (a guess).
- The scheduler repo's head (`82953c8`) isn't on GitHub; the pin is
  `5a38f5f`, which has the same scheduler code. Move the pin after a push.
- The scheduler still builds against `~/linux-src` and the Ubuntu kernel's
  type information. Pointing its build at the Kairos tree waits for
  components 2 and 4.
- This repo has no GitHub remote and no licence file yet. Kernel patches
  are GPL-2.0, like the kernel.
- `CLAUDE.md` stage 1 asks for the structs and functions involved. On
  2026-11-13 the request was for plain-word explanations in chat, without
  variable names; the names go in this log.

---

## 2. Kernel config and first boot — 2026-11-14

Options (how to choose what goes into the kernel):
- A: Linux's default config for arm64, its fragment for virtual machines,
  and a small Kairos fragment. 2,008 switches built in, 587 modules.
- B: Linux's smallest config, grown switch by switch. 382 built in.
- C: Ubuntu's config for the dev VM, trimmed. 4,087 built in, 8,299
  modules.
- D: Linux's default, cut down by hand, booting after each cut.

My pick and reasoning:
- Between A and B, and asked for a suggestion.
- For A: "ebpf will help us cover all the topics of OS, so the linux can be
  implemented beforehand".
- For B: "I will get a better understanding as we will be going one by
  one. However, I do think it could be overkill".

Verdict: B, with one adjustment: start from `allnoconfig` (every optional
switch off), not from `tinyconfig`.
- `tinyconfig` is `allnoconfig` plus the kernel's "expert" mode, which
  strips standard basics (printing, users and groups, futexes, timers), and
  it compiles for size. That suits a sensor, not a general-purpose OS.
- B gives the shortest build-and-boot loop on this VM, and every line of
  the config answers a failure that was seen.
- Not overkill: the first switches were known from a probe, and A and B are
  stored the same way (a named base plus a fragment), so backing out to A
  means changing the base's name.
- Right: B teaches more by going one by one.
- Wrong: eBPF doesn't cover all OS topics. In Kairos it covers the
  scheduling policy; boot, processes, memory, files and drivers come from
  the later components.
- Missed: the build cost on this VM, paid in every later component; and A
  doesn't turn sched_ext on until a line removes the setting that blocks it
  (reduced debug information).

Where the others fit:
- A: a kernel that must boot on hardware not known in advance; what kernel
  developers and automated testing build.
- C: systems that must stay compatible with a distribution's user space,
  modules and support.
- D: embedded products that start from a board maker's working config and
  cut it down.

Linux concepts learned:
- Kconfig: about 21,000 switches; `make allnoconfig`, `tinyconfig`,
  `defconfig`, `olddefconfig`; fragments merged with
  `scripts/kconfig/merge_config.sh`; a switch whose dependencies aren't met
  is dropped without an error. `EXPERT` selects `DEBUG_KERNEL`, and the
  debug-information choice depends on `DEBUG_KERNEL`.
- kbuild: `make O=dir Image`; `vmlinux` (ELF, 71 MB with debug info) and
  `arch/arm64/boot/Image` (6.9 MB); only what changed is recompiled.
- The device tree: QEMU's `virt` board describes itself to the kernel,
  which starts the driver matching each entry (`SERIAL_AMBA_PL011` for the
  serial port, `ttyAMA0`).
- The first file system: `BLK_DEV_INITRD`; the kernel's built-in list
  (`usr/default_cpio_list`: `/dev`, `/dev/console`, `/root`);
  `usr/gen_init_cpio.c` packs an archive from a list, without root.
- Program loaders: `BINFMT_ELF`, `BINFMT_SCRIPT`; error 8 (`ENOEXEC`)
  without them.
- `DEVTMPFS`: the kernel fills `/dev` itself.
- sched_ext's chain: `SCHED_CLASS_EXT` needs `BPF_SYSCALL`, `BPF_JIT` and
  `DEBUG_INFO_BTF`; BTF needs full DWARF debug information and `pahole`;
  `resolve_btfids` runs at link time.
- The kernel command line: `console=ttyAMA0`, `panic=-1` (reset on panic),
  `rdinit=`, `nosmp`. Power-off and reset go through PSCI.
- The order in which the kernel looks for its first program: `/init`, then
  `/sbin/init`, `/etc/init`, `/bin/init`, `/bin/sh`.

What changed: Kairos has a kernel of its own that boots. The config is two
layers: Linux's "everything off" starting point and 13 lines of ours, each
added for a failure that was seen. One script builds the kernel and stops
if the config system dropped one of our lines. A second packs the first
file system from a list. A third boots both in QEMU, and a fourth boots
unattended and checks what the console says. The shell and tools are
busybox's for now, and the first program is a short script.

What each switch fixed, in order:
- nothing of ours: no console output at all; the kernel runs and panics
  unheard.
- the serial port and console: 139 lines of messages, then "VFS: Unable to
  mount root fs".
- initramfs support: "Failed to execute /init (error -8)", "No working init
  found".
- the program loaders: a shell; `/dev` holds only `console`, "can't access
  tty; job control turned off".
- the device file system: 80 entries in `/dev`, the shell has a terminal.
- BPF, type information, sched_ext: `/sys/kernel/sched_ext/state` goes from
  missing to `disabled`.
- the `-kairos` name: `uname -r` is `7.0.0-kairos`.

How it's tested:
- `./scripts/kernel-build.sh`: "allnoconfig + 13 switch(es) of ours = 508
  switches on". The first build of the bare base took 93 s (107 MB of
  output); with debug information a full rebuild takes 196 s and the output
  is 662 MB.
- `./scripts/boot-test.sh`: six lines (kernel, init, cpus, devices,
  sched_ext, poweroff), all `ok`, exit status 0. A boot takes about 1 s.
- `./scripts/boot-test.sh --smp 16`: "16 of 16 running".
- The boot test fails when it should: `nosmp` (1 of 4 CPUs),
  `rdinit=/nonexistent` (panic), and a kernel without the `-kairos` name.
- The config check fails when it should: without the "kernel debugging"
  line, `kernel-build.sh --config-only` reports three dropped switches and
  exits with 1.
- `./scripts/kernel-check.sh` still passes; the kernel tree is unchanged.

Interview questions:
1. You built a kernel config up from nothing. What does a Linux kernel need
   before it can give you a shell, and what do you see at boot when each
   piece is missing?
2. How does the kernel know what hardware it is running on in your QEMU
   machine, and why does that make the config matter for booting?
3. You turned on a config option and it stayed off without an error. Why
   does that happen, and how did you guard against it?

Resume line: "Built a minimal Linux 7.0 kernel for Kairos from an empty
config: 13 options on top of 'all off' give a console, an initramfs, a
shell and sched_ext, and a script boots it in QEMU and checks the result in
about a second."

Open questions / next:
- Component 3: our own init (PID 1).
- The settings a scheduler feels are still the base's: 250 ticks a second,
  ticking even when idle, no precise timers. Ubuntu's kernel has 1,000,
  tickless and precise timers; the scheduler's 1 ms timer needs the last.
  For component 4. The preemption mode (lazy) already matches Ubuntu's.
- BPF's tracing support is off, which gives one build warning
  (`resolve_btfids: unresolved symbol bpf_session_is_return`). The
  scheduler's trace output would need it.
- Initramfs support brought seven decompression formats with it that the
  uncompressed archive doesn't use. They could be switched off.
- No networking, no cgroups, no NUMA, no loadable modules yet. Each goes in
  with the component that needs it.
- busybox comes from the build machine (`busybox-static` 1.37.0), so the
  archive isn't built from source yet.
- The scripts build for arm64 only.
- With patches in the series the kernel will call itself `7.0.0-kairos+`;
  the build adds the plus when the tree isn't exactly at a release tag.
- Disk: 8.1 GB free.

---

## 3. Our own init — 2026-11-14

Options (what runs as process 1):
- A: busybox's own init, driven by a short table of what to run at start,
  what to keep running and what to do at shutdown. No code of ours.
- B: a minimal init in C that does only what process 1 must: set up, start
  one shell with a terminal, collect finished processes, restart the shell,
  shut down in order. Service management is a separate program, later.
- C: a full init in C with service management inside process 1 (service
  descriptions, ordering, restart policies, LC or batch per service).

My pick and reasoning:
- A: "A seems to be good enough, since this is the core. Let us not take
  risk. Also, I dont think there is much more to learn by coding this
  through B and C."
- After the verdict: "ok lets go with B".

Verdict: B.
- "No risk in the core" is the right principle in production, where a
  crash of process 1 takes the machine down. Here a crash costs a
  one-second reboot in an emulator, the boot test catches a broken init
  before it is committed, and the old script stays in the image as a
  fallback (`rdinit=/init.fallback`).
- "Not much more to learn" was wrong for B: its code is almost all about
  processes (starting, collecting, signals, terminals, shutdown), which is
  the "processes" item of goal 1. It was partly right for C, where much of
  the code is reading files and bookkeeping.
- Right: rejecting C, which puts the most code where a bug is a panic.
- Missed: Kairos was defined in component 0 as having "our own init and
  user space". With A, user space is entirely busybox. A also can't mark a
  program as batch: this busybox has no tool for setting a scheduling
  class.

Where the others fit:
- A: systems where init isn't the point and proven code matters most:
  embedded devices, Alpine.
- C: systems whose value is in service management: systemd on the big
  distributions, Android's init, launchd on macOS.
- B's own relatives: tini in containers, and the small process 1 of runit
  and s6.

Linux concepts learned:
- Starting a program: `fork()` makes a copy, `execv()` replaces the copy's
  program. The copy sets itself up in between (signal mask, session,
  terminal, input and output).
- Finished processes: a process stays in the table (state Z,
  `EXIT_ZOMBIE`) until its parent collects it with `waitpid()`.
  `waitpid(-1, NULL, WNOHANG)` in a loop collects all that are ready.
  `ECHILD` means the caller has no children.
- Adoption: when a parent finishes first, `forget_original_parent()` and
  `find_new_reaper()` in `kernel/exit.c` give its children to process 1
  (or to a nearer process that asked for them with
  `PR_SET_CHILD_SUBREAPER`).
- The rules for process 1: `do_exit()` panics with "Attempted to kill
  init!"; `sig_task_ignored()` in `kernel/signal.c` drops a signal whose
  handling process 1 has left at the default (`SIGNAL_UNKILLABLE`), and
  SIGKILL and SIGSTOP always; `sig_ignored()` keeps any signal that is
  blocked.
- Signals without handlers: `sigprocmask(SIG_BLOCK)` to hold signals back,
  `sigwaitinfo()` and `sigtimedwait()` to take them one at a time. The mask
  is inherited across `fork()` and `execv()`, so the child lifts it.
  Several SIGCHLD can merge into one.
- The kernel command line: `unknown_bootoption()` in `init/main.c` passes
  unknown words to init as arguments, or as environment if they have "=".
  Words with a dot are taken for module settings and dropped, so user
  space reads them from `/proc/cmdline`.
- Terminals: a session (`setsid()`) is a group of processes sharing one
  controlling terminal; `ioctl(TIOCSCTTY)` takes one (`tiocsctty()` in
  `drivers/tty/tty_jobctrl.c`; session leaders only). `tty_open()` in
  `drivers/tty/tty_io.c` never makes `/dev/console` a controlling terminal.
  `/sys/class/tty/console/active` names the device behind the console (the
  last entry). `/dev/tty` is "my own terminal".
- Hang-up: when a session's leader finishes, `disassociate_ctty()` and
  `__tty_hangup()` send SIGHUP to the foreground group and cut off every
  open file on the terminal, except the ones opened through
  `/dev/console`.
- Shutdown: `kill(-1, sig)` reaches every process except process 1
  (`kill_something_info()`). The `reboot()` call (`kernel/reboot.c`:
  `kernel_power_off()`, `kernel_restart()`, `kernel_halt()`) stops the
  machine at once and tells no process. `sync()` writes out what file
  systems hold in memory.
- An interactive shell ignores SIGTERM and acts on SIGHUP.
- Static linking: `cc -static` puts the C library into the program; the
  image has no shared libraries.

What changed: Kairos's first process is our own program, 386 lines of C of
which about 200 are code. It mounts the kernel's information file systems.
It starts the shell as the leader of a new session with the serial port as
its terminal, so Ctrl-C works. It holds back four signals and sleeps until
one arrives: on "a child finished" it collects every finished process,
including the orphans the kernel handed to it, and starts a new shell if
the shell was one of them; on the three signals that `halt`, `poweroff`
and `reboot` send, it asks every process to finish, tells the shell its
session is over, waits up to 5 seconds, stops by force what is left, has
file systems write out their data, and then makes the kernel's reboot
call. busybox's tool links are made when the image is packed. The old
script stays in the image as a fallback first program.

What each step fixed, in order:
- set up and start the shell only: "can't access tty; job control turned
  off"; a finished orphan stays in state Z with parent 1; after `exit`
  nothing answers; `poweroff` does nothing.
- collecting and restarting: the orphan is gone; `exit` gives "init:
  starting the shell again"; `kill -9 1` does nothing; a shell that can't
  run is retried once a second.
- the terminal: no complaint; Ctrl-C stops `sleep 5` (before, it ran its
  full 5 seconds).
- shutdown: "init: power off: asking every process to finish", "init:
  every process has finished", "reboot: Power down".

Found on the way:
- The plan said the kernel passes `kairos.test` to init as an argument. It
  doesn't (the dot). Init reads `/proc/cmdline`.
- Plain `reboot` from the shell waited the full 5 seconds: the shell
  itself ignores SIGTERM. Init now also sends it SIGHUP.
- The self-test's background program was sometimes signalled before it
  had set up its reaction. The script now waits for a file the program
  makes when it is ready.
- The kernel's build has a folder `init/` in the build folder, so our
  program is built as `~/kairos-build/userspace/init`.
- The steps were reordered against the plan: the self-test hand-off went
  into the first step, so the boot test passed at every commit and each
  new check was seen failing first.

How it's tested:
- `./scripts/boot-test.sh`: nine lines (kernel, init, cpus, devices,
  sched_ext, leftovers, terminal, shutdown, poweroff), all `ok`, exit
  status 0. Passed 8 of 8 runs with 1, 2, 4, 8 and 16 CPUs.
- Each new check failed before its change: `leftovers` reported 1 with
  the init that didn't collect; `terminal` and `shutdown` failed with the
  inits before those steps.
- In a shell with commands fed over the serial port: `exit` gives a new
  shell; `kill -9 1` leaves process 1 running; Ctrl-C works, also in a
  restarted shell; `poweroff`, `reboot` ("Restarting system") and `halt`
  ("System halted") work; a program that catches SIGTERM prints its
  message before the power goes off; one that ignores it is stopped after
  5 seconds ("some are still running after 5 s; stopping them").
- `./scripts/run-qemu.sh rdinit=/init.fallback` gives a shell with busybox
  as process 1. The boot test fails on it, after its 120 s limit.

Interview questions:
1. What is a zombie process, why does the kernel keep it, and why is
   collecting them a job for process 1 in particular?
2. In your OS, `poweroff` first did nothing while `poweroff -f` worked.
   Why, and what does your init do between the request and the machine
   stopping?
3. Your shell started without a terminal and Ctrl-C did nothing. What has
   to be set up between creating the process and running the shell, and
   why couldn't the shell just use the console that init was given?

Resume line: "Wrote the init (process 1) of Kairos in about 200 lines of C:
it gives the shell a controlling terminal, reaps orphaned processes,
restarts the shell, and shuts the system down in order; a QEMU boot test
checks each behaviour on every run."

Open questions / next:
- Component 4: the scheduler loaded and supervised by init. Init starts
  exactly one program today. The loader is a second kind of child, with
  its own rule for what happens when it ends.
- Shutdown doesn't unmount anything. Nothing is on a disk yet; with a
  root file system on a disk (component 9) it has to.
- The 5 seconds are fixed, and init doesn't say which processes it had to
  stop by force.
- Init doesn't react to Ctrl-Alt-Del or a power button; the kernel sends
  process 1 a signal for each, and it hasn't asked for them.
- One shell on one terminal, as root, with no login.
- Init is 696 KB because it carries the C library. The kernel tree's own
  small library (`tools/include/nolibc`) would make it a few KB.
- No licence file yet, so init.c has no licence line; the kernel's style
  checker warns about that and nothing else that applies to user space.
- Disk: 8.1 GB free.
