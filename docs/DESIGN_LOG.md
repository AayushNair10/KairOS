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
| 1 | Source layout: how Linux and the scheduler live in the repo | how kernel changes are carried: tags, patch series, forks | none |
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
