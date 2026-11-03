/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions shared by the BPF policy (scx_kairos.bpf.c) and the loader
 * (scx_kairos.c).
 */
#ifndef __SCX_KAIROS_H
#define __SCX_KAIROS_H

#define KAIROS_MAX_CPUS 64

/*
 * Fixed-point 1.0 for the late share. 2^20, not 1024: an integer EWMA with
 * weight 1/2^k can't decay below 2^k - 1 (avg >> k rounds to 0), which at
 * x1024 and k = 6 is 6.2%, above the 5% threshold.
 */
#define LATE_ONE (1ULL << 20)

/* Wakeup preemption modes (RESEARCH_LOG section 3.2). */
enum kairos_mode {
	MODE_BATCH = 0,		/* never preempt on wakeup; long slices */
	MODE_LATENCY = 1,	/* a woken task may preempt; short slices */
};

/* --split: which borrower a woken LC task preempts (--borrower-pick). */
enum kairos_pick {
	PICK_FIRST,	/* the first LC CPU in CPU order running one */
	PICK_PREV,	/* the woken task's previous CPU if it runs one, else first */
	PICK_LONGEST,	/* the one that has run longest since it started */
};

/* Event counters in the per-CPU "stats" map; the loader sums the CPUs. */
enum kairos_stat {
	STAT_LOCAL,		/* woken task got an idle CPU in select_cpu */
	STAT_GLOBAL,		/* task inserted into the shared queue */
	STAT_PREEMPT,		/* woken task preempted a busy CPU */
	STAT_PREEMPT_BATCH,	/* ... whose current task was classified batch */
	STAT_NO_VICTIM,		/* LATENCY mode, no idle CPU and nothing to preempt */
	STAT_TO_BATCH,		/* a task's class changed to batch */
	STAT_FROM_BATCH,	/* ... and back */
	STAT_BORROW,		/* --split: batch task started on an LC CPU */
	STAT_KICK,		/* queued task woke an idle CPU (kick_idle) */
	STAT_DELAYED,		/* --preempt-wait-us: LC wakeup waited instead of preempting */
	STAT_LATE_PREEMPT,	/* ... and was still waiting after it: borrower preempted */
	STAT_SYNC,		/* --wake-sync: woken task queued on the waker's CPU */
	KAIROS_NR_STATS,	/* not NR_STATS: vmlinux.h has one */
};

/* Why a task was waiting for a CPU. */
enum kairos_ready {
	READY_WAKEUP,		/* woken up (SCX_ENQ_WAKEUP) */
	READY_PREEMPTED,	/* put back before its slice ran out */
	READY_SLICE_END,	/* put back when its slice ran out */
	READY_OTHER,		/* runnable some other way (e.g. a new task) */
	KAIROS_NR_READY,
};

/*
 * Diagnostics (--diag): every wait for a CPU and every run, per task group.
 * Groups are task-name (comm) prefixes from --diag-comm, plus one for all
 * other tasks. Waits are counted in buckets by length; the bounds are in us.
 */
#define DIAG_NAMED	5			/* groups named by --diag-comm */
#define DIAG_GROUPS	(DIAG_NAMED + 1)	/* + everything else */
#define DIAG_BOUNDS_US	50, 200, 1000, 2000, 5000, 10000, 20000
#define DIAG_BUCKETS	8			/* one more than the bounds */
#define DIAG_COMM_LEN	16			/* the kernel's TASK_COMM_LEN */

struct diag_group {
	__u64 waits[KAIROS_NR_READY][DIAG_BUCKETS];
	__u64 wait_ns[KAIROS_NR_READY];
	__u64 runs;		/* times it started running */
	__u64 run_ns;		/* CPU time */
	/*
	 * Runs that began by preempting a CPU (the task's next run after a
	 * wakeup that preempted), by how long they lasted, in the wait
	 * buckets; and their total length.
	 */
	__u64 preempt_runs[DIAG_BUCKETS];
	__u64 preempt_run_ns;
};

struct diag {
	struct diag_group g[DIAG_GROUPS];
};

#endif /* __SCX_KAIROS_H */
