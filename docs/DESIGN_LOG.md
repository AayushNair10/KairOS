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
| 4 | Kairos Scheduler loaded and supervised by init (done) | the bpf() syscall, struct_ops attach, BTF; what an ejection does | config only |
| 5 | A service manager that sets task classes (done; the shell was split off, see 5b) | cgroups, scheduling policies, event loops | none (a scheduler change on its `kairos-os` branch) |
| 5b | Our own shell (optional, not scheduled) | parsing, pipes, process groups, job control | none |
| 6 | Task class as a kernel concept (done: a cgroup controller of our own, read by BPF) | syscall entry, task_struct, user ABI, kfuncs | patch (0001 to 0004) |
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

---

## 4. Kairos Scheduler loaded and supervised by init — 2026-11-18

Options (all need the same kernel switches and the same build of the
policy; they differ in who holds the scheduler and who reacts when it goes
away):
- A: init starts the loader, unchanged, as a second child beside the
  shell, and starts it again by a rule written in init.
- B: init is the loader. The BPF library and the compiled policy are
  linked into init, which attaches the scheduler before any other process
  and holds it.
- C: a one-shot loader attaches the scheduler, pins it in the BPF file
  system and ends. No process holds it; a watcher reads the kernel's state
  file and loads it again after an ejection.
- D: a separate supervisor program of ours between init and the loader,
  holding the restart rule.

My pick and reasoning:
- B: "B makes the most sense for me, since the loader being in init feels
  right. Since init is called before, the loader also gets called."
- After the verdict: "ok lets go with A".

Verdict: A.
- "Init runs first, so the loader does too" is true, but just as true of
  A, C and D: under A the loader is the first thing init starts. The order
  comes from what init starts first, not from what is inside init.
- "Feels right" pointed the wrong way. The scheduler being central is a
  reason to keep it running, not to put it in process 1, the one program
  whose crash is a kernel panic.
- Missed, B's costs: the BPF library is 37,900 lines in 21 files, all of
  which would run in process 1, and the loader handles errors by printing
  a message and exiting, in 7 places; in process 1, exiting is a panic.
  The first failure of step 1 ("Function not implemented") would have
  been a panic under B. Under A it was one line on the console.
- What B really buys is small here. Under A only init and the loader run
  before the scheduler attaches, and component 8 takes over early attach.
  B's one real gain was that init would know why the scheduler was thrown
  out; the loader then ended with status 0 after an ejection, as after a
  clean stop. Step 4 closed that gap with an exit status.

Where the others fit:
- B: a single-purpose appliance whose init is the only long-lived program.
  From memory, systemd's process 1 loads small BPF programs through the
  same library for per-service restrictions. No system I know of puts a
  scheduler there.
- C: BPF programs with no user-space half, such as filters and counters.
  From memory, Android pins them at boot with a one-shot loader.
- D: systems with many services. s6 and runit keep a tiny process 1 above a
  supervisor tree, and distributions run sched_ext schedulers as a
  systemd service.

Linux concepts learned:
- Loading: the loader makes `bpf()` calls for the maps
  (`BPF_MAP_CREATE`) and the programs (`BPF_PROG_LOAD`: the verifier
  checks every path, the JIT compiles it). The table of callbacks is a map
  of type `BPF_MAP_TYPE_STRUCT_OPS`. `BPF_LINK_CREATE` goes through
  `bpf_struct_ops_link_create()` (`kernel/bpf/bpf_struct_ops.c`) to
  `bpf_scx_reg()` and `scx_enable()` (`kernel/sched/ext.c`).
- Lifetime through a handle: the link is a file descriptor. When its last
  copy closes, by `close()` or because the process was killed,
  `bpf_scx_unreg()` calls `scx_disable(SCX_EXIT_UNREG)`. A pin in the BPF
  file system is the other way to hold it.
- The fallback: `scx_disable_workfn()` first switches to bypass mode (the
  kernel queues tasks itself and stops calling the policy), then moves
  every task back to the class of its own policy (`scx_setscheduler_class()`:
  EEVDF for ordinary tasks), then lets the policy go.
- When the kernel unloads a BPF scheduler by itself (`enum scx_exit_kind`
  in `kernel/sched/ext_internal.h`):
  - the link closes (UNREG);
  - the policy breaks a rule that the kernel checks as it runs: an invalid
    CPU, a queue that doesn't exist, a task sent to a CPU it may not run on
    (`scx_error()`, ERROR);
  - the policy asks: `scx_bpf_error()` (ERROR_BPF) or `scx_bpf_exit()`
    (UNREG_BPF). Kairos Scheduler calls the first only while starting
    (more CPUs than it supports, or its queues or timer can't be made);
  - the watchdog: a runnable task waited longer than the timeout, 30 s
    unless the policy asks for less (`check_rq_for_timeouts()`, from
    `scx_watchdog_workfn()`, ERROR_STALL), or the watchdog itself didn't
    run (`scx_tick()`);
  - an RCU stall (`scx_rcu_cpu_stall()`, called from
    `kernel/rcu/tree_stall.h`), or a soft or hard lockup (`scx_softlockup()`
    and `scx_hardlockup()`, from `kernel/watchdog.c`; Kairos has no lockup
    detector);
  - a CPU coming or going, when the policy has no callbacks for that
    (`handle_hotplug()`, UNREG_KERN with a restart code; Kairos has no CPU
    hot-plug);
  - SysRq-S (`sysrq_handle_sched_ext_reset()`, SYSRQ; Kairos has no SysRq).
  - Suspend doesn't unload it: `scx_pm_handler()` only bypasses it
    meanwhile (Kairos has no suspend).
- A policy can't hang or crash the kernel: the verifier proved that its
  memory accesses are safe and that its loops end. It can make bad choices,
  which is what the watchdog is for.
- Types by name: the policy names kernel fields, and they are matched
  against the running kernel's BTF at load time. The loader reads the exit
  kinds' values the same way (`__COMPAT_ENUM_OR_ZERO` in
  `tools/sched_ext/include/scx/compat.h`).
- What the BPF library needs from the kernel: `memfd_create()` for
  placeholder maps (`create_placeholder_fd()` in `tools/lib/bpf/libbpf.c`);
  socket filters for its feature probes (`bpf_object__probe_loading()`;
  `probe_kern_global_data()` in `tools/lib/bpf/features.c`, whose failure
  silently leaves out the maps for global variables). The helper
  `bpf_get_current_task_btf()` is in `kernel/trace/bpf_trace.c`, built only
  with `CONFIG_BPF_EVENTS`. The tools' `SCX_OPS_OPEN` names the cgroup
  callbacks, which exist only with `CONFIG_EXT_GROUP_SCHED`.
- The tick: a slice that has run out is noticed on the next tick
  (`task_tick_scx()` then `resched_curr()`), so a busy task runs up to one
  tick past its slice. `CONFIG_HZ` sets the rate; `HIGH_RES_TIMERS` lets a
  timer fire at its own time (resolution in `/proc/timer_list`);
  `NO_HZ_IDLE` stops the tick on an idle CPU.
- The kernel's state for user space: `/sys/kernel/sched_ext/state` and
  `/sys/kernel/sched_ext/root/ops`. The kernel also sends a uevent with
  `SCXOPS=` when a scheduler is enabled; init doesn't listen for it.
- Exit status: `waitpid()` gives `WIFEXITED()` and `WEXITSTATUS()` for a
  program that ended itself, `WIFSIGNALED()` and `WTERMSIG()` for one a
  signal killed. `sigtimedwait()` waits for a signal with a deadline.

What changed: Kairos boots with Kairos Scheduler running. The loader is
built from the pinned kernel tree as one static program and packed into
the image. Init starts it before anything else, in a session of its own
with no terminal, and waits until the kernel reports the scheduler enabled
(at most 10 s) before it starts the shell. The loader holds the scheduler
through a file descriptor, so whenever it ends the kernel puts every task
back on EEVDF. On a branch of the scheduler repository, the loader now ends
with a status that says why: 0 stopped on request, 1 didn't start, 3 the
kernel unloaded it without an error, 4 after an error, 5 the watchdog.
Init acts on it: after 0 or 3 it leaves the scheduler stopped; otherwise it
starts it again, at once if it had run 60 s, or else after 1, 2, 4 and 8 s,
and gives up after five such short runs in a row. While it waits, it keeps
answering signals. `kairos.noscheduler` on the kernel's command line boots
without it. The kernel config gained 14 lines: what the BPF library and the
policy need, and precise timers, 1,000 ticks a second and no tick on idle
CPUs, each measured first.

What each step did, in order:
1. Build the loader: `scripts/scheduler-build.sh` builds the BPF library,
   bpftool and a header of the kernel's types from the pinned tree, then
   the loader, statically linked (2.9 MB). It didn't compile against our
   kernel until group scheduling was on. On Kairos it stopped at "failed to
   initialize skeleton BPF object 'scx_kairos': -38".
2. Kernel switches, one failure at a time: in-memory files and
   `memfd_create`; then "Couldn't load trivial BPF program" (networking);
   then "program of this type cannot use helper
   bpf_get_current_task_btf#158" (BPF tracing). Then it attached by hand,
   and Ctrl-C detached it.
3. Timers and tick, measured before and after: timer resolution 4 ms, then
   1 ns; the policy's 1 ms timer fired at most 250 times a second, then
   about 870. Two busy tasks on one CPU with a 3 ms slice ran 4.0 ms at a
   time at 250 ticks a second and 3.7 ms at 1,000. No tick on idle CPUs:
   about 785 timer interrupts a second on each idle CPU before, fewer than
   6 after.
4. The loader, on the scheduler repository's branch `kairos-os`: `--quiet`
   and the exit statuses. Statuses 0, 1 and 3 seen on the VM's own kernel
   (3 with SysRq-S).
5. Init starts the loader before the shell: checks `scheduler` and
   `loader`.
6. The restart rule: check `restart`, which kills the loader.
7. The off switch: the boot test switches its four scheduler checks when
   given the word.

Found on the way:
- The plan said the policy compiled against Kairos's types and the loader
  linked statically. Each was true alone; together, the loader didn't
  compile until the cgroup switches were on.
- Removing networking as unneeded gave "fd 9 is not pointing to valid
  bpf_map": the BPF library's probe for global variables failed and it
  quietly left out their maps. Networking went back in.
- A 3 ms slice runs about 4 ms even at 1,000 ticks a second: it has a few
  microseconds left at the third tick, so it ends on the fourth. The kernel
  the scheduler was evaluated on gives 4.0 ms too. Recorded in the
  scheduler's research log on `kairos-os`.
- With precise timers, QEMU sometimes prints "hrtimer: interrupt took N
  ns": the emulator stalled. Harmless.
- A second loader started by hand fails with "Device or resource busy":
  the kernel takes one BPF scheduler at a time.
- In this kernel, an unload without an error (status 3) comes only from
  SysRq-S, the policy asking, or CPU hot-plug, and in the last case the
  loader restarts itself without ending.
- One kernel build failed once ("Deleting file 'kernel/bpf/btf.o'"); the
  rerun passed. Cause not found.

How it's tested:
- `./scripts/boot-test.sh`: 13 lines (kernel, init, cpus, devices,
  sched_ext, scheduler, loader, leftovers, terminal, restart, shutdown,
  unload, poweroff), all `ok`, exit status 0. Passed with 1, 2, 4, 8 and
  16 CPUs. The scheduler attached 0.6 to 1.7 s after init started the
  loader and was running again 1.3 to 1.6 s after its loader was killed.
- `./scripts/boot-test.sh kairos.noscheduler`: the same 13 lines; the four
  about the scheduler expect it off. Passed with 1, 4 and 16 CPUs.
- Each new check failed first: `scheduler` and `loader` with the old
  init, `restart` with the init that didn't restart, and the four
  off-switch checks with the init before the off switch.
- In the shell: `kill -9` on the loader 4 s after boot brings it back
  after 1 s ("1 of 5"); a plain `kill` leaves it stopped; killed after
  62 s it is started again at once, and the next early kill shows "1 of
  5" again; with a loader started by hand holding the kernel's place,
  init's starts end with "didn't start" after 1, 2, 4 and 8 s, and it
  gives up after the fifth; `poweroff` during a wait is answered at once.
  With `kairos.noscheduler`: the state is `disabled`, no loader runs, and
  one started by hand attaches but isn't init's to look after.
- With scratch images: no loader file ("couldn't be run" five times, then
  init gives up and the shell works); a loader that never attaches (init
  goes on after 10 s); a stand-in that fails at once.
- Not seen from the real scheduler: an unload after an error or by the
  watchdog (statuses 4 and 5). Init's handling of them was tried with the
  stand-in.

Interview questions:
1. Your scheduler is a BPF program and its loader is killed with
   SIGKILL. What happens to every task on the machine, and why doesn't the
   kernel need the loader's help to do it?
2. Why does your init start the loader as a child and restart it, instead
   of loading the BPF program itself? How does it decide whether to start
   it again?
3. A 3 ms slice ran for about 4 ms even with 1,000 ticks a second. Why,
   and what would make slices end on time?

Resume line: "Made Kairos boot with its own sched_ext scheduler: built the
BPF loader as one static binary for a minimal kernel, traced each load
failure to a missing kernel feature, and wrote init's supervision of it (a
restart rule driven by the loader's exit status, with back-off and a
boot-time off switch); a QEMU boot test checks attach, restart and
fallback on every run."

Open questions / next:
- Push: the scheduler repository's branch `kairos-os` (a40a1ff) isn't on
  GitHub yet, and this repo's submodule pin needs it. That repository's
  `main` also has one unpushed commit.
- After a stop on request nothing starts the scheduler again but a
  reboot, and a loader started by hand isn't supervised. Component 5's
  service manager is the place for both.
- Init looks at the state file every 10 ms while it waits at boot. The
  kernel's uevent could tell it instead.
- Statuses 4 and 5 haven't been seen from the real scheduler.
- The cost of no tick on idle CPUs to wake-up latency isn't measured;
  neither is whether the 4 ms slice changes the scheduler's evaluated
  results.
- Networking and BPF tracing are in only for the BPF library and one
  helper; they add about 3.8 MB to the kernel image.
- From component 3: shutdown doesn't unmount; no licence file.
- Disk: 6.3 GB free.

---

## 5. A service manager that sets task classes — 2026-11-21

Split before the options: the roadmap's component 5 was "shell and a
service manager". The two share nothing but the name, and nothing later
depends on our own shell, so the shell became an optional component 5b
and busybox's shell stays.

Options (all with a description per service and its class, LC or batch):
- A: services inside init: init's table grows from the scheduler and the
  shell to every service, with a control channel into process 1.
- B: a separate service manager, started and restarted by init; the
  class is the thread's scheduling policy (batch or idle), which the
  scheduler already reads; a subreaper gets detached orphans back.
- C: like B, but each service in a control group of its own, under one
  group per class; the scheduler takes the class from the group (a
  change on its `kairos-os` branch).
- D: busybox's runit tools, rebuilt from source with `chrt`: a run script
  per service, no C of ours.

My pick and reasoning:
- "I am confused between B and C. It all depends on whether LC and batch
  are supposed to be classified differently. If so, we can go ahead with
  C. If not, B is the best solution."
- After the verdict and plan: "yes".

Verdict: C.
- Right: the question that separates B and C (is the class its own
  concept, or Linux's scheduling policy reused?), set up as "if yes, C".
- Missed: answering it. It is yes: the policy already means something to
  EEVDF (batch doesn't preempt on wakeup; idle gets a weight of 3 against
  1,024), and Kairos runs on EEVDF in its first second, in back-offs and
  with the off switch; the class belongs to the service, not to each
  thread, which can change its own policy with one call; and component 6
  makes the class a kernel concept.
- Missed: tracking. Knowing which processes belong to a service favours
  groups whatever the class is: a detached daemon, stopping all of a
  service, counting its CPU time.
- B's real strengths: no change to the scheduler, and less code. The
  evaluated rule stays available: the group rule is a new option, and
  `--main-config` is unchanged.
- A correction to my options: "changing a running service's class: move
  one group" was wrong. Version 2 groups can't be renamed or moved
  (`kernel/cgroup/cgroup.c`, "rename(2) is disallowed"); the class changes
  by restarting the service in the other group.

Where the others fit:
- A: small systems with a fixed set of services: the `respawn` lines of
  System V init and busybox's init.
- B: supervisors of foreground services without grouping: s6, runit.
- C: systemd (a control group per service), container runtimes.
- D: Void Linux (runit as init and supervisor), embedded busybox systems.

Linux concepts learned:
- Control groups, version 2 (`kernel/cgroup/cgroup.c`): one tree, a file
  system mounted at `/sys/fs/cgroup`; a group is a folder. Writing a
  process number to `cgroup.procs` moves the process with all its
  threads; "0" means the writer. A forked child starts in its parent's
  group. `cgroup.subtree_control` switches controllers on for a group's
  children ("+cpu"). `cgroup.events` says "populated 1" while the group or
  one below it holds a process; a change wakes `poll()` with POLLPRI
  (`cgroup_file_notify()`, `kernfs_notify()`, `kernfs_generic_poll()`).
  `cgroup.kill` kills everything in the group at once, including what is
  forked meanwhile. `cpu.stat` counts CPU time in every group. Groups
  can't be renamed or moved. A group's id is its folder's inode number on
  a 64-bit machine (`kernfs_id_ino()`).
- The no-internal-processes rule (`cgroup_vet_subtree_control_enable()`):
  a group with controllers on for its children may not hold processes,
  except the root. It binds domain controllers (memory, I/O), not threaded
  ones such as cpu, which split CPU between a group's processes and its
  children themselves.
- kernfs counts a newly opened file as changed until it is first read
  (`atomic_set(&on->event, 1)` in `fs/kernfs/file.c`): read, then poll.
- The scheduler's view of groups is the CPU controller's: a task's
  `sched_task_group`, its nearest group the CPU controller applies to.
  sched_ext passes it to `ops.init_task()` (`args->cgroup`, with
  `CONFIG_EXT_GROUP_SCHED`) and to `ops.cgroup_move()`.
  `scx_bpf_task_cgroup()` may only be called with the task's run-queue
  lock held, which `init_task()` doesn't have.
- BPF: `bpf_cgroup_ancestor()` is `KF_RCU`: it takes only a trusted
  pointer, such as a callback's argument. A pointer read from a field of
  an argument isn't one ("R1 must be a rcu pointer"); plain loads through
  it are allowed, so read the id and take a reference with
  `bpf_cgroup_from_id()`.
- EEVDF and the policies (`kernel/sched/fair.c`): "BATCH and IDLE tasks do
  not preempt others"; idle's weight is 3 (`WEIGHT_IDLEPRIO`); a thread
  may ask for its own slice, 0.1 to 100 ms (`sched_attr.sched_runtime`,
  `__setparam_fair()`). No label is set by the kernel itself.
- One wait for everything: `signalfd()` turns signals into a file
  descriptor, so `poll()` waits on signals, a named pipe and the groups'
  event files together. A named pipe opened for reading and writing never
  reports "every writer has gone"; opened for writing with O_NONBLOCK and
  no reader, it fails with ENXIO; a write of fewer than PIPE_BUF bytes
  arrives whole. `rename()` replaces a file in one step.
- `clock_nanosleep()` to an absolute time keeps a periodic schedule from
  drifting. CLOCK_MONOTONIC is the same clock in every process.
- `cgroup_disable=cpu` on the kernel's command line switches a controller
  off.
- Under QEMU without KVM, each emulated CPU is a thread on the host; with
  more emulated CPUs than real ones, timing measures the host's sharing.

What changed: init mounts the control groups, makes `lc` and `batch` and
switches the CPU controller on, then starts Kairos Scheduler with a new
class rule from its `kairos-os` branch: a task is batch if it is in the
batch group or below it (or has lowered its own policy), LC otherwise.
The policy notes the class when a task is created and when it moves, so
a wakeup costs nothing extra. Init then starts the service manager, a new
program of 930 lines, and restarts it if it ends. The manager runs each
service in a group of its own under its class; a service runs while its
group holds a process and has ended once the group is empty, whatever
its processes did. It restarts services with init's back-off rule, finds
services that are already running when it starts, and answers `service
status`, `start` and `stop` through a named pipe and a state file per
service; stopping sends TERM to the group and empties it with
`cgroup.kill` after 5 s. `batch COMMAND` runs one command in a fresh
group below `batch`. Two demo services: `probe` (LC, a new 150-line
program that measures its own wake-up delays) and `crunch` (batch, a
busy loop per CPU, started on request).

What each step did, in order:
1. Control groups at boot (`f8154dd`): mount, `lc`, `batch`, CPU
   controller.
2. Class by group (`04c9517` on `kairos-os`, then `05c58c4`, `674fe8e`):
   the scheduler reads the group; init passes the rule, or falls back to
   the policy rule if the groups couldn't be made.
3. The service manager and the probe (`478a030`).
4. Control: status, start, stop, start-on-request services, `batch`
   (`32905a4`).
5. The end-to-end measurement in the boot test (`0a80fea`).
Plus the README's design section with diagrams (`ea8415e`).

Found on the way:
- The plan said a group with controllers on for its children can't hold
  processes. True only for domain controllers; cpu is threaded.
- The verifier refused `bpf_cgroup_ancestor()` on `init_task()`'s
  `args->cgroup` ("R1 must be a rcu pointer"); fixed by looking the group
  up by id first.
- A service started on request was taken for ended at once: its group's
  freshly opened events file counted as changed, `poll()` woke before the
  new process had joined, and the manager read "populated 0". Fixed by
  reading the file once after opening it, and by calling a service ended
  only if its group is empty and it had begun (a process was seen in the
  group, or its first process has finished).
- `service start` of an unknown service waited 10 s for a state that
  never came; the command now checks for the description first.
- In emulation, EEVDF gave the probe shorter delays (192 us median, crunch
  "as batch", 4 CPUs) than Kairos Scheduler did (234 to 578 us). A guess,
  not checked: preempting a task on another CPU means an inter-processor
  interrupt, slow under emulation, while EEVDF preempts on the CPU where
  the probe's timer fired. This probe is not the evaluation's workload.

How it's tested:
- `./scripts/boot-test.sh`: 19 lines, all `ok`. Passed with 1, 2, 3, 4, 8
  and 16 CPUs, and with 4 and 2 CPUs and `kairos.noscheduler`. New checks,
  each seen failing first: `cgroups`, `loader` (now also the class rule),
  `services`, `respawn`, `stop`, `batch`, `class`.
- `class`: with crunch busy on every CPU, the probe's median wake-up delay
  as batch / as LC / without crunch: 1 CPU 273 / 2686 / 403 us; 2 CPUs
  170 / 702 / 358; 3 CPUs 437 / 1952 / 333; 4 CPUs 578 / 2499 / 305,
  430 / 2180 / 399 and 234 / 2297 / 402. The check wants as batch at most
  half of as LC. Its negative control: booted with `cgroup_disable=cpu`
  (classes from the policy only), 2370 / 1696 us, and `class` fails. Not
  judged with more emulated CPUs than real ones (8 CPUs: 3472 / 3935 us)
  or under EEVDF.
- The scheduler's own counters, loader started by hand with `--stats`:
  4 busy loops in the root group, 4 LC CPUs and 0 borrows a second; moved
  into `batch`, 2 LC CPUs and 2 borrows; moved back, 4 and 0; started in
  `batch` before the scheduler loaded, 2 and 2 to 3.
- In the shell: `service status`, `start` and `stop`; a service whose
  first process leaves behind one that ignores TERM is stopped after
  5.1 s; killing only a service's first process leaves the service
  running; after `kill -9` on the manager, a new one adopts the running
  services; a failing service is retried after 1, 2, 4 and 8 s and given
  up; `batch` returns the command's exit status and removes its group;
  `cgroup_disable=cpu` makes init start the scheduler with the policy
  rule.

Interview questions:
1. Your service manager decides that a service has ended when its control
   group is empty, not when the process it started exits. Why, and what
   goes wrong with the simpler rule?
2. How does your BPF scheduler know whether a task is LC or batch, when
   does it find out, and why does a process the service forks a minute
   later get the same class?
3. In your emulated test, Linux's own scheduler gave the probe shorter
   delays than yours. What would you need before trusting a latency
   comparison between two schedulers, and why can't QEMU without hardware
   acceleration give it?

Resume line: "Built a service manager for Kairos in C that runs each
service in its own cgroup v2 group under a latency-critical or batch
parent, tracks services by group occupancy rather than PIDs, restarts
them with back-off and stops them with cgroup.kill; extended the
sched_ext scheduler to classify tasks by cgroup through its init_task and
cgroup_move callbacks, and checked the whole path in a QEMU boot test (an
LC program's median wake-up delay 4 to 10 times shorter when the CPU hog
is labelled batch, in emulation)."

Open questions / next:
- Component 6, task class as a kernel concept. The class is now a naming
  convention (being below the folder `batch`); component 6 could make it
  a kernel interface, for instance a file in each control group, or a
  system call.
- Push: step 4, the README's design, step 5 and this entry aren't on
  GitHub yet. The scheduler's `kairos-os` branch (04c9517) is.
- The EEVDF result above is unexplained; component 12 (measurement at
  real speed) is the place to compare.
- A running service's class changes only by restarting it in the other
  group; moving its processes isn't implemented.
- From memory, not tried: the kernel can start a process directly in a
  group (`CLONE_INTO_CGROUP`, through glibc's posix_spawn), which would
  remove the moment a new service process runs outside its group.
- With the CPU controller on, EEVDF shares CPU between groups by weight
  whenever Kairos Scheduler isn't running. Not measured.
- Services' logs grow in memory without limit; init restarts a failing
  manager once a second forever; services have no start order; nothing
  checks who sends commands (everything runs as root).
- The policy half of the class rule (a task lowering itself to batch)
  wasn't tried: the image has no `chrt`.
- From component 3: shutdown doesn't unmount; no licence file.
- Disk: 4.7 GB free (6.3 GB at the end of component 4).

---

## 6. Task class as a kernel concept — 2026-11-23

Options:
- A: reuse `cpu.idle`: batch means "the group is idle"; no kernel patch.
  EEVDF honours it too; two values only; sched_ext isn't told which
  groups are idle when it loads.
- B: a class on each task, set by a new system call (or `prctl`), copied
  at fork, read as one field.
- C: a `cpu.class` file in the CPU controller, kept in its per-group
  record (`struct task_group`), with a new sched_ext callback when it
  changes.
- D: a small controller of our own, `kairos`, with `kairos.class` in every
  group, inherited by the groups below, and a kernel function for BPF.

My pick and reasoning:
- "D, because classification is very important."
- After the verdict and plan: "yes".

Verdict: D.
- Right pick, but the reason doesn't choose between the options: all four
  make the class something the kernel knows, and A and C keep it per
  group too.
- What decides it: who reads the class, and per what. Memory is charged
  to groups and reclaim picks groups; disk bandwidth is shared between
  groups. So components 10 and 11 need "the class of this group", from
  outside the scheduler. A and C keep it in the CPU controller's record,
  whose type is private to `kernel/sched/` and which exists only where
  the CPU controller is on; B keeps it on tasks, and a page doesn't
  belong to a task. The unit is a service, which is a group, so a relabel
  should be one write.
- Missed, D's costs: upstream wouldn't take it. The cgroup v2 document
  names "controllers which existed only to identify membership" among
  version 1's problems, version 1's `net_cls` (a class id per group) was
  dropped for v2, and `include/linux/cgroup_subsys.h` says "DO NOT ADD ANY
  SUBSYSTEM WITHOUT EXPLICIT ACKS FROM CGROUP MAINTAINERS". Upstream's
  answer is BPF matching the group itself, which is component 5. EEVDF
  ignores the class, so with the scheduler off LC and batch are alike, as
  before. And it is the most code of the four.

Where the others fit:
- A: batch next to latency-sensitive work on a stock kernel. From memory:
  `cpu.idle` was added by Google engineers in Linux 5.15 for that, and
  systemd exposes it as `CPUWeight=idle`.
- B: when threads of one process need different classes and the label
  must follow a thread through several subsystems: XNU's per-thread QoS
  classes (read by its scheduler, timer coalescing and disk throttling);
  Linux's own I/O priority (`ioprio_set`).
- C: a setting that concerns only the CPU: how Linux added `cpu.weight`,
  `cpu.idle` and `cpu.uclamp.min/max` (from memory: Android sets uclamp on
  its foreground and background groups).
- D's precedent: version 1's `net_cls`.

Linux concepts learned:
- A controller (`struct cgroup_subsys`, listed in
  `include/linux/cgroup_subsys.h`, which gives it `kairos_cgrp_id`): one
  record per group (`struct cgroup_subsys_state`, the css), made and
  ended through `css_alloc`, `css_online`, `css_offline`, `css_free`.
  `css_online()` runs under `cgroup_mutex` (`online_css()`) once the css
  is linked into its parent's children; `css_free()` runs after an RCU
  grace period, so lockless readers are done.
- `implicit_on_dfl`: on in every group without `cgroup.subtree_control`,
  as perf_event is; such a controller must also be `threaded` (a
  `WARN_ON` in `cgroup_init()`). It is hidden from `cgroup.controllers`
  (`cgroup_control()` masks `cgrp_dfl_implicit_ss_mask`) and "+kairos" is
  refused (ENOENT). `/proc/cgroups` lists only version 1 controllers.
- Files: a `struct cftype` array (`dfl_cftypes`), named
  `<controller>.<file>`; `seq_show` to read, `write` to write;
  `CFTYPE_NOT_ON_ROOT`. `sysfs_match_string()` parses a word, ignoring
  the newline. The files of a new group belong to whoever made it
  (`current_fsuid()` in `cgroup_add_file()`, `cgroup_mkdir()`).
- A task's records: `task->cgroups`, a `struct css_set` shared by every
  task in the same groups, one css per controller; `task_css()` under RCU
  (or with preemption off: `task_css_set_check()` accepts
  `rcu_read_lock_sched_held()`).
- Locking: `cgroup_mutex` nests outside kernfs active protection
  (`cgroup_kn_lock_live()`), and group removal waits for active files to
  drain under `cgroup_mutex` (`kernfs_drain()`). So a file's write handler
  must not take `cgroup_mutex` directly; ours takes its own mutex, as the
  legacy freezer does. Lockdep shows the cycle only if a removal really
  drains: `kernfs_drain()` skips the wait, and its lockdep annotation,
  when no one has the file open or active.
- Walking descendants: `css_for_each_descendant_pre()` under RCU,
  `css_rightmost_descendant()` to skip a subtree (cpuset does this).
  WRITE_ONCE/READ_ONCE for values read without a lock.
- kfuncs: `__bpf_kfunc`, `BTF_KFUNCS_START/END`, `BTF_ID_FLAGS`,
  `register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, ...)` from a
  `late_initcall` (sched_ext's own in `scx_init()`). In 7.0 pointer
  arguments must be trusted by default; `KF_TRUSTED_ARGS` is gone ("must
  be referenced or trusted" in the verifier). The function shows in BTF
  as a FUNC with a `bpf_kfunc` decl tag.
- Fork order (`copy_process()`, `kernel/fork.c`): `cgroup_fork()` points
  the child at `init_css_set`; `sched_cgroup_fork()` (sched_ext's
  `init_task`) and `sched_post_fork()` (`enable`) run before
  `cgroup_post_fork()` moves it into its parent's groups; then
  `wake_up_new_task()`.
- libbpf matches a `__ksym` function's types by kind only: any enum
  matches any enum, an int doesn't match an enum
  (`__bpf_core_types_are_compat()`, `btf_kind_core_compat()`). A
  `__weak` kfunc that is missing makes `bpf_ksym_exists()` false and the
  verifier drops the guarded call.
- Debugging builds: `CONFIG_PROVE_LOCKING` (lockdep, with PROVE_RCU) and
  `CONFIG_DEBUG_ATOMIC_SLEEP`; checkpatch's FILE_PATH_CHANGES warning;
  `scripts/get_maintainer.pl -f FILE`.

What changed: the kernel now keeps a class for every control group. Four
patches add a controller, `kairos` (`kernel/cgroup/kairos.c`, 260
lines): each group but the root has `kairos.class` (`lc`, `batch`, or
`inherit`, the default) and `kairos.class.effective`. A write recomputes
the class of every group below that inherits, under the controller's own
lock, so a read is one load with no lock. A new group copies its
parent's class when it comes online. Kernel code reads a task's class
with `kairos_task_class()`, BPF with `bpf_kairos_task_class()`. Kairos
Scheduler's new `--class=kernel` asks for the class at each decision, so
relabelling a running service is one write, with no restart and no
callback; it still loads on kernels without the function. Init writes
`batch` to the batch group at boot and starts the scheduler in that mode,
falling back to component 5's rule on a kernel without the controller.

What each step did, in order:
1. The boot test's checks first, failing on the old kernel (`67dd69e`).
2. Patch 0001, the controller and its files (`6dec3e0`); the style check
   leaves out checkpatch's MAINTAINERS warning (`018e581`).
3. Patch 0002, passing a write down; 0003, the documentation
   (`e089f43`); a lockdep build and `--kernel FILE` for the QEMU scripts
   (`515e464`).
4. Patch 0004, the BPF function (`a9be5cb`).
5. `--class=kernel` and a victim fix on the scheduler's `kairos-os`
   branch (`03bbbfa`, `10d0b72`), pinned here (`f1b5614`).
6. Init sets batch's class and uses the new mode (`0d34328`).

Found on the way:
- The deadlock the controller's own lock avoids, shown by lockdep when the
  write was changed to take `cgroup_mutex` as a test: a write holds the
  file active and takes `cgroup_mutex`; `rmdir` holds `cgroup_mutex` and
  waits for the file. Reported only with the file held open during the
  removal (see `kernfs_drain()` above).
- `pick_victim()` judged a victim's class by run time under
  `--class=cgroup`. Reached only without `--split`, which Kairos doesn't
  use; fixed.
- A one-off 4 s probe run can vary 3x in emulation: on the old kernel,
  where both measurements had crunch as batch, they gave 875 and 284 us.
- The plan's "two readers" became one: nothing calls a group reader yet.

How it's tested:
- `./scripts/boot-test.sh`: 21 lines, all `ok`, at 1, 2, 4 and 8 CPUs,
  with `kairos.noscheduler`, and on the lockdep build (no reports). New
  checks, seen failing first: `kclass`, `relabel`; `loader` and `class`
  changed. `class`, with crunch relabelled LC by one write while it runs,
  as batch / as LC / without crunch: 1 CPU 166 / 2401 / 357 us; 2 CPUs
  254 / 637 / 432; 4 CPUs 291 / 2569 / 189; 8 CPUs not judged; under
  EEVDF 409 / 368 / 393 (the class is ignored, as expected).
- On the kernel without our patches (`--kernel
  ~/kairos-build/Image.component5`): init says "this kernel keeps no task
  classes" and starts `--class=cgroup`; only the four class checks fail.
- By hand: a tree `batch` > `a` > (`b`, `c` = lc > `d`): a write to
  `batch` reached `a` and `b`, not `c` and `d`; `inherit` on `c` made `c`
  and `d` follow. 400 writes racing 100 mkdir/rmdir on the lockdep build:
  no reports.
- The scheduler by hand (`--main-config --class=kernel`, crunch below
  `batch`): batch 336 us median, 2 LC CPUs, ~540 borrows a second; `echo
  lc` to crunch's group while it ran: 2414 us, 4 LC CPUs, 0 borrows;
  `inherit`: 479 us, 2, ~530. Without the split (`--force-preempt`): 471
  of 472 preemptions on batch tasks, then 0 of 489 once crunch was LC.
- `./scripts/kernel-check.sh`: 4 patches, rebuild, series and style ok.
  The BTF of the built kernel has `bpf_kairos_task_class` with the
  `bpf_kfunc` tag and `enum kairos_class` (0, 1, 2).

Interview questions:
1. You made the class a cgroup controller rather than a field on each
   task set by a system call. Who reads the class, and why does that
   favour a group? And why would the Linux maintainers probably still
   reject your controller?
2. Your write handler takes the controller's own mutex instead of
   `cgroup_mutex`. What deadlock does that avoid, and why did lockdep not
   report the bad version until you held the file open?
3. A new task and a new group: when does each get its class, and what
   goes wrong if the scheduler reads it in `init_task()`, or the
   controller copies the parent's class in `css_alloc()`?

Resume line: "Added a cgroup v2 controller to Kairos's Linux kernel (a
4-patch series, checkpatch-clean) that gives every control group an
inherited latency-critical or batch class, read without locks and
exported to BPF through a kfunc; the sched_ext scheduler reads it at each
scheduling decision, so relabelling a running service takes one write.
Checked the locking with lockdep and the whole path in a QEMU boot test."

Open questions / next:
- `service status` still shows the class from the service's description
  (its parent group), not the kernel's; after `echo lc >` a service's
  `kairos.class` it is wrong. Either status reads
  `kairos.class.effective`, or a `service class NAME lc|batch` command
  writes the file; then a service's place in the tree no longer has to
  encode its class.
- Raising a class: a group may set `lc` under a `batch` parent, and a new
  group's files belong to whoever made it, so a user given a delegated
  subtree could lift work out of batch. A rule like the policy's ("a task
  can lower itself, not raise itself") would make the effective class the
  stricter of a group's own and its parent's.
- EEVDF ignores the class. Setting `cpu.idle` on `batch` too would keep
  LC ahead when the scheduler is off, at the risk of starving batch.
- A "class of a group" reader, for components 10 and 11, when they need
  it.
- Only two classes; the numbers are part of the BPF interface.
- checkpatch's SPDX check is skipped: it needs the `python3-ply` package
  (not installed; ask first).
- Push: this repository's last two commits (init's change and this
  entry). The scheduler's `kairos-os` branch is on GitHub.
- Disk: 3.6 GB free; the lockdep build takes 1.1 GB in
  `~/kairos-build-debug`.

