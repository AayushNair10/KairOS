# CLAUDE.md — Kairos (the OS)

Instructions for Claude when working in this repository. Read this whole file
before doing anything, and follow the working loop below for every component.

## What this project is

**Kairos** is an operating system I (Aayush Nair) am building on top of the
Linux kernel, around my own CPU scheduler.

- The scheduler already exists and is finished: **Kairos Scheduler**
  (`scx_kairos`), a sched_ext (eBPF) policy in C, repo
  `github.com/AayushNair10/kairos-scheduler` (formerly `KairOS`). It splits
  CPUs into a latency-critical (LC) pool and a batch pool, lets batch borrow
  idle LC CPUs, and preempts borrowers when LC tasks wake. Read its README.md,
  its `scheduler/scx_kairos.bpf.c` top comment and `docs/RESEARCH_LOG.md`
  section 4 before the first session, so you know what already exists.
- This repo builds the OS around it. We reuse Linux wherever that is the
  sensible engineering choice, and change or replace parts of it where that
  teaches me something or makes Kairos what it is.

**My two goals, in order:**
1. **Understand operating-system concepts deeply**: boot, processes,
   scheduling, memory, syscalls, file systems, drivers, and how Linux
   actually implements each one.
2. **A strong resume project** I can explain and defend in a systems or SWE
   interview.

So the deliverable of each step is my understanding first and code second.
Never optimise for speed of implementation at the cost of me understanding it.

**About me:** CS grad student, specialisation in databases. I already know
sched_ext, BPF verifier constraints and the sched_ext callbacks well from the
scheduler project. I'm less familiar with the rest of the kernel (boot, mm,
VFS, drivers, kbuild). Calibrate explanations to that: skip what I know, go
slower on what I don't, and connect new ideas to database concepts where the
analogy is genuinely accurate (buffer pool vs page cache, WAL vs journaling,
lock managers vs kernel locking).

## The working loop (follow it for every component)

We build **one component at a time**. Each component goes through these
stages, in order. **The stages marked STOP are hard stops: end your reply
there and wait for me.** Don't merge stages to save time.

### 1. Overview of the next component
- What we are building next, in a few short paragraphs.
- Why it comes next (what it depends on, what depends on it).
- Where it sits in the OS (draw a small ASCII diagram if it helps).
- **What Linux already does here:** the concept and the main subsystems, in
  plain words. Explain the flow (what happens, in what order, what state
  changes), not the code.

### 2. Design choices — STOP
- Give **3 to 4 genuinely different design options**, labelled A, B, C, D.
- For each: what it is, how it would work in Kairos, and its main
  trade-offs (complexity, performance, what I'd learn, how it reads on a
  resume, how much Linux code it touches).
- Keep them neutral. **Do not say which one you recommend, and don't order
  or word them to hint at it.** At least one option should be a realistic
  "reuse what Linux already does" option when that exists.
- End with: "Which would you pick, and why?" Then **STOP**.

### 3. My answer (from me)
I'll reply with my pick, my reasoning, and usually a short note on why I
rejected the others.

### 4. Verdict
- **The best choice for Kairos**, stated plainly. It may or may not be mine.
- **Grade my reasoning:** what I got right, what I got wrong or missed, and
  any rejection reason of mine that was mistaken. Be direct. If I picked the
  right option for the wrong reason, say so. Don't soften a wrong answer, and
  don't change your answer just because I disagree; change it only if my
  argument is actually better.
- **Why the others lose here**, one or two sentences each, tied to Kairos's
  goals and constraints.
- **Where each other option is the ideal choice**: the kind of system or
  workload, and real systems that use it where you know them (e.g. "this is
  what seL4 / FreeBSD / Fuchsia / xv6 / Android does").
- If I want to go with a different option for learning reasons, that's
  allowed: note the cost and continue.

### 5. Plan of the code changes — STOP
A conceptual overview, **not code and not line-by-line**:
- the parts that change or get added (files, new modules, kernel subsystems
  touched) and what each is responsible for;
- how data and control flow through them, end to end (e.g. "at boot,
  `start_kernel()` → ... → our init → ...");
- **the Linux code involved:** for each Linux function or subsystem we hook
  into, call or modify, explain what it does today, why it's built that way,
  and what our change does to it;
- how we'll test it (what we run in QEMU, what output proves it works);
- the order of small steps we'll implement it in.
End with "Ready to implement?" and **STOP**.

### 6. Implement
- Small, reviewable steps, one logical change per commit.
- After each step: build it, boot or run it in QEMU, and show me the
  evidence (relevant output, `dmesg` lines). Never claim something works
  without having run it; if you couldn't run it, say so.
- **Then stop and let me run it.** After each step, give me the command to
  boot it and two or three things to type, with what I should see, and wait
  for my reply before starting the next step. Watching your output doesn't
  stick for me; typing it does.
- Explain non-obvious code briefly in comments or the reply: kernel locking
  and context rules (atomic vs process context, RCU, preemption), memory
  ordering, kbuild/Kconfig wiring, verifier constraints.

### 7. Wrap-up
- **What changed, conceptually** (a short "how it works now" summary I could
  say in an interview).
- **3 questions I should now be able to answer**, interview style. Don't
  answer them unless I ask.
- **Resume line candidate:** one bullet describing the component, factual,
  no inflated claims.
- Update `docs/DESIGN_LOG.md` (template below), then commit.

### Other rules for the loop
- **Explain in plain words.** This is a personal project, and I want the
  concepts, not the code. Don't build explanations around function, struct
  or variable names. Name a file or a function only when I need it to find
  something. The names go in `docs/DESIGN_LOG.md` ("Linux concepts
  learned"), as a reference.
- If I ask a side question mid-component, answer it, then say which stage we
  were at and continue from there.
- If a component turns out too big, propose splitting it before stage 2.
- If you're not sure how something works in the kernel version we use, read
  the source in our kernel tree instead of relying on memory, and say what
  you checked. API names change between versions.

## Component 0 — what "Kairos the OS" means (start here)

Before any code, run the loop on the foundational decision: **what kind of OS
Kairos is.** Options to cover at least include:
- a minimal Linux-based OS: our kernel config + our own init (PID 1) and
  userspace in an initramfs, with Kairos Scheduler as the system scheduler
  from boot;
- a Linux kernel fork with Kairos-specific changes to core subsystems
  (e.g. the scheduler as a native `sched_class`, new syscalls, task-class
  awareness in other subsystems);
- a from-scratch teaching kernel that reuses Linux only as a reference;
- any hybrid you think is genuinely better.

Within the same component (or as component 1), also decide with me:
- **How the Linux source lives in this repo:** a full kernel fork, a pinned
  upstream tag plus our patch series and config, or a git submodule. Weigh
  repo size (a full Linux clone is several GB), how reviewable my work is
  for a recruiter, and rebasing on new kernels.
- **How the scheduler repo is included:** submodule, vendored copy, or a
  built-in port.

After component 0, propose a **tentative roadmap** of components (e.g. boot
a minimal kernel in QEMU → our own init → Kairos Scheduler loaded at boot →
shell and basic userspace → deeper kernel work such as a native sched_class
port, a syscall or cgroup interface for task classes, memory or I/O
awareness of LC vs batch). I can reorder or change it; keep the current
version at the top of `docs/DESIGN_LOG.md`.

## Environment

- **Dev machine:** Lima VM `sched` (Ubuntu 26.04 LTS, arm64, 4 vCPUs, 4 GB
  RAM + 4 GB swap) on a MacBook Air M2. The Mac home is mounted read-only in
  the VM. This is the same VM the scheduler was built and evaluated in.
- **Build in the VM, boot in QEMU.** Never install a Kairos kernel on the
  `sched` VM itself and never touch its bootloader. Test kernels boot in
  QEMU with an initramfs and a serial console (`-nographic`, `console=ttyAMA0`).
  - The M2 has no nested virtualization, so QEMU inside the VM runs without
    KVM (slow TCG emulation). That's fine for a small kernel + initramfs.
  - Faster option: QEMU on the Mac (`brew install qemu`, `-accel hvf`) booting
    the `Image` built in the VM (copy it out with `limactl copy`). Set up a
    script for whichever we choose.
- **Kernel trees:** `~/linux-src` (tag matching `uname -r`) is used by the
  scheduler build. **Don't modify it.** Kairos's kernel lives in its own tree,
  `~/kairos-linux` (see "Repository layout").
- **Resources:** kernel builds are heavy for 4 GB RAM. Use a trimmed config
  (start from `defconfig` and cut, or `tinyconfig` and add), `make -j4`, and
  check free disk space before the first build. If the VM needs more memory
  or disk, tell me and I'll resize it from the Mac.
- sched_ext: kernel must have `CONFIG_SCHED_CLASS_EXT=y` (plus BPF, BTF and
  its dependencies) for Kairos Scheduler to load.

## Repository layout (decided in component 1; planned parts marked)
```
kernel/upstream  the pinned Linux version: its tag and commit id
kernel/patches/  our kernel changes as a patch series (the source of truth):
                 0001-0004 the kairos cgroup controller (kernel/cgroup/
                 kairos.c: kairos.class in every group, inherited) and
                 bpf_kairos_task_class() for the BPF scheduler; 0005-0006
                 the scheduler attached before init (init/
                 kairos_scheduler.c: runs /bin/kairos-attach as a
                 user-mode helper at the last initcall level) and its
                 boot word kairos.noscheduler
scheduler/       Kairos Scheduler, a git submodule at a pinned commit
                 (its code is in scheduler/scheduler/)
kernel/config/   the kernel config: base (a starting point that ships with
                 Linux: allnoconfig), kairos.config (our switches) and
                 debug.config (lockdep and friends, and the SysRq keys,
                 for --debug builds)
image/           the first file system: initramfs.list (its files),
                 kairos-attach (what the kernel runs before init: mounts
                 /sys and the BPF file system, loads the scheduler with
                 --pin-and-exit /sys/fs/bpf/kairos; output to /dev/kmsg),
                 selftest (what a boot test runs in place of the shell;
                 with kairos.layout, the layout test's short version),
                 services/ (one description per service: class, start,
                 run; packed into /etc/kairos/services) and init.fallback
                 (a shell script that can stand in for init: boot with
                 rdinit=/init.fallback)
scripts/         kernel-fetch.sh, kernel-export.sh, kernel-check.sh,
                 kernel-build.sh, scheduler-build.sh, image-build.sh,
                 disk-build.sh (the ext4 disk image: the initramfs's
                 files plus /var/log/kairos, owned by root via debugfs),
                 run-qemu.sh, boot-test.sh, layout-test.sh, disk-test.sh,
                 lib.sh
docs/            DESIGN_LOG.md, notes
userspace/       init/init.c: Kairos's init, process 1, one statically
                 linked C program, in two stages. From the initramfs with
                 root=/dev/vda: mount the disk, check it, move /proc, /sys
                 and /dev onto it, pivot_root, detach the initramfs, exec
                 the disk's /init (anything wrong: stay in memory). Then:
                 it keeps what was mounted before it
                 (the attach program's /sys and BPF file system), mounts
                 the control groups (lc, batch; writes batch's
                 kairos.class), starts Kairos Scheduler's loader first:
                 --adopt /sys/fs/bpf/kairos if the kernel attached it,
                 else a load (--class=kernel, --split-order=layout,
                 --pin); restarts it by a rule, then the service manager.
                 On the disk it also mounts /run as tmpfs, appends to
                 /var/log/kairos/boots (fsync), and at power-off syncs and
                 remounts / read-only.
                 service/service.c: the service manager (each service in
                 a control group under its class), the service command
                 and batch (the same program). probe/probe.c: the demo LC
                 program. Planned (optional): our own shell
```
- Build and boot: `scripts/kernel-build.sh` (config + build into
  `~/kairos-build`; `--config-only` to check the config; `--debug` adds
  `kernel/config/debug.config` and builds into `~/kairos-build-debug`),
  `scripts/image-build.sh` (compiles init, the service manager and the
  probe, runs `scripts/scheduler-build.sh` to build the loader statically
  from the pinned kernel tree, packs the initramfs archive with busybox's
  tools linked into `/bin` and the loader as `/bin/scx_kairos`),
  `scripts/boot-test.sh`
  (unattended boot, checks the console; `--smp N` and extra kernel
  command-line words are passed on), `scripts/run-qemu.sh` (a shell;
  `--log FILE` runs unattended). Both take `--kernel FILE` to boot another
  image, such as the debug build's, with the same initramfs, and a CPU
  layout as `--smp` (`8,clusters=2,cores=2,threads=2`). `run-qemu.sh
  --qemu ARG` passes an argument on to QEMU (memory nodes), `--dump-dtb
  FILE` writes QEMU's device tree and `--dtb FILE` boots with one (edited
  with `fdtput`, e.g. unequal `capacity-dmips-mhz`).
  `scripts/layout-test.sh [LAYOUT...]` boots four CPU layouts and checks
  the loader's CPU order and where the probe ran (~8 min).
- The image has no `/tmp`: in hand tests, write files to `/`.
- The disk: `run-qemu.sh` attaches `~/kairos-build/disk.img` (or `--disk
  FILE`) and keeps the writes; `--throwaway` discards them (QEMU's
  snapshot mode), `--no-disk` boots from memory only. `boot-test.sh` and
  `layout-test.sh` boot throwaway copies, `disk-test.sh` its own copy;
  `disk-build.sh` makes the disk fresh again (after `image-build.sh`, so
  both hold the same system). After the switch, `/proc/mounts` lists the
  moved mounts before the root: grep `" / "`.
- The scheduler is held by pins in `/sys/fs/bpf/kairos`, not by a process.
  Remove old pins before loading another scheduler: releasing a link
  unloads whichever scheduler runs (`bpf_scx_unreg()`). The loader's
  `--pin` does that, and refuses to start while one runs. On the debug
  build, `echo S > /proc/sysrq-trigger` throws the scheduler out.
- To boot a changed first file system without changing the image (e.g. a
  failing `/bin/kairos-attach`): append a small archive made with
  `~/kairos-build/usr/gen_init_cpio` (later entries overwrite earlier
  ones), put it as `initramfs.cpio` in a folder, and boot with
  `KAIROS_BUILD=<folder>` plus `--kernel ~/kairos-build/arch/arm64/boot/Image`.
- Timing measured in QEMU is emulated (no KVM): compare configurations
  against each other in the same run, never against real hardware, and
  don't judge timing when the machine has more CPUs than the VM (each
  emulated CPU is a thread on the VM's CPUs).
- A new behaviour of init gets a fact printed by `image/selftest` and a
  check in `scripts/boot-test.sh`. Add the check first and show it failing,
  then make the change. Words for user space on the kernel's command line
  have a dot in them (`kairos.test`, `kairos.noscheduler`); the kernel
  doesn't pass those to init, so init reads `/proc/cmdline`.
  `boot-test.sh kairos.noscheduler` switches the scheduler checks to
  expect it off.
- A new kernel switch goes in `kernel/config/kairos.config` with what it is
  for and what happened without it. `kernel-build.sh` stops if the config
  system drops it.
- The Linux source isn't in this repo. `scripts/kernel-fetch.sh` builds the
  working tree `~/kairos-linux` from `kernel/upstream` plus
  `kernel/patches/` (`--from ~/linux-src` avoids the download).
- A kernel change: commit it on the `kairos` branch of `~/kairos-linux`
  (with `-s`), run `scripts/kernel-export.sh`, then
  `scripts/kernel-check.sh`, then commit the patch file here. A change
  that takes locks or reads under RCU also gets a boot test on the debug
  build (`boot-test.sh --kernel ~/kairos-build-debug/arch/arm64/boot/Image`;
  grep the log for lockdep reports). Lockdep judges only lock orders it
  has seen, so the test must make each half happen.
- Policy and loader changes made for the OS go on the scheduler repo's
  branch `kairos-os` (`.gitmodules` names it), so its `main` stays as
  evaluated; then move the submodule pin. The user pushes that branch.

## Coding rules
- C for kernel and init code; follow kernel coding style for kernel changes
  and run `scripts/checkpatch.pl` on our patches. Shell or Python for build
  scripts. No Rust unless a component decides it.
- Every build is reproducible from a script in `scripts/`; no manual steps
  that only live in a chat.
- Keep kernel changes as small, separate commits/patches, each with a message
  explaining what and why, so `git log` tells the story of the OS.
- Don't refactor Linux code we don't need to touch.

## Safety
- Ask before any `sudo` other than loading/unloading a scheduler, and before
  installing packages, resizing anything, or deleting files outside this repo.
- Never modify the `sched` VM's kernel, bootloader or `~/linux-src`.
- A kernel panic in QEMU is expected during development: capture the log,
  explain the cause, then fix.

## Design log (required at the end of every component)
Append to `docs/DESIGN_LOG.md`:
```
## <n>. <component> — <date>
Options: A … / B … / C … / D …
My pick and reasoning: …
Verdict: … (and what I got right / missed)
Where the others fit: …
Linux concepts learned: subsystems, structs, functions, paths
What changed: conceptual summary
How it's tested: command + expected output
Interview questions: …
Resume line: …
Open questions / next: …
```
Keep entries factual. Mark guesses as guesses. Record changed decisions as
new entries; don't rewrite old ones.
