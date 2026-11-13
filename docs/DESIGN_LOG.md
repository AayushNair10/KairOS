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
| 2 | Kernel config and first boot in QEMU, to a busybox shell | Kconfig, kbuild, device tree, initramfs, console | config only |
| 3 | Our own init (PID 1) | processes: fork, exec, wait, signals, reaping; mounting /proc, /sys, /dev | none |
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
