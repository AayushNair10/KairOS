/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_kairos (KairOS): load-adaptive wakeup preemption.
 *
 * Built on scx_simple's policy (Linux v7.0 tools/sched_ext): one shared
 * queue ordered by weighted vtime, and a woken task that finds an idle CPU
 * goes straight to it. Added so far:
 *
 * - A load signal. The number of runnable tasks, the wakeup count and the
 *   time tasks wait between becoming ready and running are counted in the
 *   callbacks; a periodic BPF timer turns them into EWMAs (fast and slow
 *   load, wakeup rate, mean wait). User space only reads them.
 * - Two modes from the same code (RESEARCH_LOG section 3.2):
 *     BATCH:   a woken task that finds no idle CPU waits in the shared queue
 *              (scx_simple's behaviour); long slices.
 *     LATENCY: it preempts a busy CPU instead (pick_victim()); short slices.
 *   Fixed by --never-preempt / --force-preempt, or, with --adaptive,
 *   switched by the timer against a latency target (update_mode()): woken
 *   tasks that waited longer than lat_target_ns are "late"; too many late
 *   wakeups in BATCH mode switch to LATENCY at once, and LATENCY mode tries
 *   BATCH again only after a hold time that doubles each time BATCH fails
 *   quickly (RESEARCH_LOG section 3.4, revised 2026-10-30).
 * - Run time is charged from timestamps (running -> stopping) instead of the
 *   slice left over, which missed time when the kernel refilled the slice of
 *   a task that kept running (RESEARCH_LOG section 4).
 * - A first piece of task classification (Feature A, RESEARCH_LOG 3.5): a
 *   task that has used more than batch_min_run_ns of CPU since it last went
 *   to sleep is "batch". With --victim batch, LATENCY mode only preempts
 *   CPUs running a batch task, so request threads are not preempted in the
 *   middle of a request (E1b, 6.4).
 * - A soft CPU split (--split; RESEARCH_LOG 2.10), used instead of the two
 *   modes: the CPUs in split_cpus are divided into LC CPUs (for latency-
 *   critical tasks) and batch CPUs, each with its own queue. Each pool
 *   serves its own queue first and lends its CPUs to the other when that
 *   queue is empty. A woken LC task takes an idle CPU, LC CPUs first, else
 *   preempts a batch task borrowing an LC CPU, else queues; batch tasks on
 *   batch CPUs are never preempted. The timer sizes the LC pool from LC
 *   tasks' CPU use and queue plus a buffer (update_split()). A prediction
 *   of LC demand written by the loader (--ml-model) can grow the pool
 *   early; it is ignored once older than ml_ttl_ticks.
 * - Diagnostics (--diag): every wait for a CPU, by why the task was waiting
 *   (woken, preempted, slice ran out) and by task group (task-name
 *   prefixes), to see which waits the wakeup-only latency signal misses.
 *
 * The vtime parts are scx_simple's: Copyright (c) 2022 Meta Platforms, Inc.
 * and affiliates; Tejun Heo <tj@kernel.org>; David Vernet <dvernet@meta.com>.
 */
#include <scx/common.bpf.h>
#include "scx_kairos.h"

char _license[] SEC("license") = "GPL";

/*
 * Tunables, set by the loader before loading (see its --help). The verifier
 * sees them as constants, so e.g. the printk below is removed entirely when
 * trace_enqueue is false.
 */
const volatile bool trace_enqueue;
const volatile bool trace_class;	/* printk every batch class change */
const volatile u32 forced_mode = MODE_BATCH;
const volatile u64 lat_slice_ns = 3000000;	/* slice in LATENCY mode */
const volatile u64 batch_slice_ns = 20000000;	/* slice in BATCH mode */
/* A task that slept may start at most this far behind vtime_now. */
const volatile u64 sleep_credit_ns = 20000000;
const volatile u64 timer_interval_ns = 1000000;
const volatile u32 fast_shift = 3;	/* EWMA weight 1/8 per timer tick */
const volatile u32 slow_shift = 6;	/* EWMA weight 1/64 per timer tick */
/* LATENCY mode preempts only CPUs whose current task is classified batch. */
const volatile bool victim_batch_only;
/*
 * Batch classification: a task is batch once it has run for this much CPU
 * time since it last went to sleep. Time spent waiting for a CPU doesn't
 * count, so a request thread stuck in a long queue doesn't become batch.
 * Request threads sleep after each request, and Go parks a worker thread
 * (a kernel sleep) after ~10 ms of running even under a backlog; zstd
 * never sleeps. (Replaces a per-stop EWMA of voluntary stops, which marked
 * busy request threads batch under overload: RESEARCH_LOG 6.5, 6.6.)
 */
const volatile u64 batch_min_run_ns = 20000000;
/*
 * Adaptive mode switching (--adaptive). A wakeup is "late" if the woken task
 * waited longer than lat_target_ns for a CPU. late_frac is an EWMA of the
 * late share of wakeups, x LATE_ONE. Only tasks on signal_cpus count (bit i =
 * CPU i), so e.g. a load generator on its own CPU doesn't drive the mode.
 */
const volatile bool adaptive;
const volatile u64 lat_target_ns = 1000000;
const volatile u64 late_max = LATE_ONE * 5 / 100;	/* 5% */
/*
 * late_frac's weight per timer tick is 1/2^late_shift: with 6 and a 1 ms
 * tick it averages over ~64 ms. At 1/8 (~8 ms, 50-100 wakeups) a few
 * unlucky ticks crossed 5% while whole seconds stayed below 1% (E2 pilot,
 * RESEARCH_LOG 6.8).
 */
const volatile u32 late_shift = 6;
const volatile u64 hold_min_ns = 200000000;	/* LATENCY at least this long */
const volatile u64 hold_max_ns = 5000000000;
const volatile u64 signal_cpus = ~0ULL;
/*
 * Task class. Default: the run-time rule above (is_batch()). With
 * class_policy, batch = the SCHED_BATCH and SCHED_IDLE policies, set by the
 * user (e.g. chrt --batch 0), as for Linux's own scheduler.
 */
const volatile bool class_policy;
/*
 * Soft CPU split (--split). split_rank[cpu] is the CPU's position among the
 * split CPUs (-1: not in the split; such CPUs act as LC CPUs). The first
 * nr_lc split CPUs are LC CPUs. nr_lc stays within [lc_min, lc_max]; the
 * timer sets it to ceil(LC demand + lc_buffer), in CPUs x 1024.
 */
const volatile bool split;
const volatile s32 split_rank[KAIROS_MAX_CPUS];
const volatile u32 lc_min = 1, lc_max = 1;	/* the loader sets lc_max */
const volatile u64 lc_buffer = 512;		/* half a CPU */
/* Shrink the LC pool only if it would still shrink with this much more demand. */
const volatile u64 lc_hyst = 256;		/* a quarter CPU */
const volatile u32 lc_shift = 6;		/* LC demand EWMA: ~64 ms */
const volatile u64 borrow_slice_ns = 3000000;	/* batch task on an LC CPU */
/* A woken LC task may preempt a borrower; else it waits for the borrow slice. */
const volatile bool borrow_preempt = true;
/*
 * Which borrower to preempt (--borrower-pick; RESEARCH_LOG section 9,
 * direction 5): the first LC CPU in CPU order running one (so CPU 0 is
 * preempted most), the woken task's previous CPU if it runs one (its
 * cache may still be warm there), else the first; or the one that has
 * run longest since it started (closest to the end of its slice).
 */
const volatile u32 borrower_pick = PICK_FIRST;	/* enum kairos_pick */
/*
 * Per-CPU LC queues (--lc-queue percpu; RESEARCH_LOG section 9, direction
 * 4), an ablation of the shared LC_DSQ. A queued LC task (no idle CPU, no
 * borrower preempted) waits in its CPU's own queue, LC_PCPU_DSQ(cpu); that
 * CPU serves it before batch work, other CPUs only when they would go
 * idle. Linux places a woken task on one CPU's run queue too and moves it
 * only by load balancing; this is stricter (no periodic balancing), so it
 * bounds what serving LC tasks from one shared queue is worth.
 */
const volatile bool lc_percpu;
/*
 * Delayed borrower preemption (--preempt-wait-us; RESEARCH_LOG section 9,
 * direction 2). 0: a woken LC task that finds no idle CPU preempts a
 * borrower at once. Otherwise it waits in LC_DSQ, where an LC CPU that
 * frees up (its LC task sleeps, or a borrow slice ends) takes it, and
 * only if it is still waiting after preempt_wait_ns is a borrower
 * preempted for it (wait_timerfn()). Fewer preemptions, longer waits.
 */
const volatile u64 preempt_wait_ns;
/*
 * With preempt_wait_ns: a task whose runs average at least this long
 * (from waking to sleeping, avg_run) still preempts at once; 0 = none
 * does. Meant for short runs (~10 us) of Go runtime threads, which caused
 * most preemptions in a smoke test, against 500 us request handlers.
 */
const volatile u64 preempt_min_run_ns;
/* avg_run's EWMA weight per run: 1/2^avg_run_shift. */
const volatile u32 avg_run_shift = 2;
/*
 * Work conservation. An idle CPU doesn't look at our shared queues until
 * something wakes it, so a task inserted into one while a CPU it may run
 * on is idle would wait for nothing. This happens after a preemption: the
 * preempted task is re-queued a few us after select_cpu() found no idle
 * CPU, and meanwhile the CPU of the task that did the waking may have
 * gone idle. With kick_idle (--kick-idle), enqueue() wakes such a CPU.
 * Off by default: in E2k (2026-11-01) the kicks fired ~3,800/s but changed
 * neither batch throughput nor idle time, and cost a little latency.
 */
const volatile bool kick_idle;
/*
 * Sync wakeups (--wake-sync; RESEARCH_LOG section 9, direction 3). With
 * SCX_WAKE_SYNC the waker says it is about to sleep (e.g. a pipe or
 * socket write followed by a read). scx_bpf_select_cpu_dfl() then keeps
 * the wakee on the waker's CPU when that CPU has nothing queued and the
 * system isn't saturated, instead of waking another, idle CPU; --split's
 * own CPU choice ignored the flag (E7: 15.2 vs 1.6 us per pipe round trip
 * on 2 CPUs). Off by default, so earlier configs keep their behaviour.
 */
const volatile bool wake_sync;
/*
 * ML prediction (--ml-model): the loader writes ml_demand and then ml_stamp
 * (= nr_timer_ticks at the time). A prediction at most ml_ttl_ticks old can
 * raise the LC demand the pool is sized from, never lower it. 0 = off.
 */
const volatile u64 ml_ttl_ticks;
/* Diagnostics: per-group wait and run counters (struct diag). */
const volatile bool diag;
/* Group i is the tasks whose name starts with diag_comm[i]; "" = unused. */
const volatile char diag_comm[DIAG_NAMED][DIAG_COMM_LEN];

UEI_DEFINE(uei);

#define SHARED_DSQ 0
#define LC_DSQ 1	/* --split: latency-critical tasks */
#define BATCH_DSQ 2	/* --split: batch tasks */
#define LC_PCPU_DSQ(cpu) (16 + (u64)(cpu))	/* --lc-queue percpu */

/* vmlinux.h has no #defines; the values are from uapi/linux/sched.h */
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef PF_EXITING
#define PF_EXITING 0x00000004	/* include/linux/sched.h */
#endif

static u64 vtime_now;
static u32 nr_cpus;

/*
 * State read by the loader (through the skeleton's bss) and by the timer.
 * The counters are updated with atomic adds from every CPU.
 */
u32 mode;
s64 nr_runnable;	/* tasks runnable now (queued or running) */
u64 nr_wakeups;		/* cumulative */
u64 wait_sum_ns;	/* cumulative time from ready to running */
u64 wait_cnt;
u64 wait_max_ns;	/* longest wait since the loader last reset it */
/* Written by the timer only. Loads are runnable tasks per CPU x 1024. */
u64 fast_load, slow_load;
u64 wake_rate;		/* wakeups per second */
u64 wait_avg_ns;	/* mean wait per timer period */
u64 nr_timer_ticks;
/* Latency signal (atomic adds in running()) and mode state (timer only). */
u64 lat_wakeups, lat_late;	/* cumulative, signal CPUs only */
u64 late_frac;			/* EWMA of late share, x LATE_ONE */
u64 nr_mode_switches;
u64 mode_ns[2];			/* time spent in each mode */
u64 hold_ns;			/* current LATENCY hold before trying BATCH */
static u64 mode_since;
/* --split state: lc_run_ns by atomic adds in stopping(), the rest by the timer. */
u32 nr_lc;			/* current number of LC CPUs */
u64 lc_run_ns;			/* cumulative CPU time of LC tasks on split CPUs */
u64 lc_demand;			/* EWMA of LC demand, CPUs x 1024 */
u64 lc_cpus_ns;			/* integral of nr_lc over time, for its average */
u64 nr_lc_changes;
u64 lc_queued_ticks;		/* sum over ticks of LC tasks queued (for ML) */
/* Written by the loader (user space): the predicted LC demand. */
u64 ml_demand;			/* CPUs x 1024 */
u64 ml_stamp;			/* nr_timer_ticks when written; 0 = none yet */
u64 ml_fresh_ticks, ml_raised_ticks;	/* ticks it was fresh / raised demand */
/* Preemptions by the CPU that was preempted (atomic adds). */
u64 preempt_on[KAIROS_MAX_CPUS];
/*
 * Sampled by the timer each tick (sample_idle()): idle CPUs among the split
 * CPUs, and how many of them could have run a task waiting in one of our
 * queues, both integrated over time (CPUs x ns).
 */
u64 idle_cpus_ns, idle_queued_ns;

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, KAIROS_NR_STATS);
} stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct diag);
} diag_map SEC(".maps");

/* Per task: timestamps for wait time and run-time charging; class. */
struct task_ctx {
	u64 ready_at;	/* when it last became ready to run; 0: not waiting */
	u64 running_at;	/* when it last started running; 0: not running */
	u64 run_since_sleep;	/* CPU time run since it last slept */
	u64 slice_at_run;	/* its slice when it last started running */
	u8 ready_kind;		/* why it is waiting (enum kairos_ready) */
	u8 group;		/* diagnostics group, set in running() */
	u8 preempted_in;	/* --diag: its next run starts by preempting */
	u8 delayed;		/* waits in LC_DSQ for preempt_wait_ns (STAT_DELAYED) */
	u64 avg_run;		/* EWMA of CPU time from waking to sleeping */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctxs SEC(".maps");

/*
 * Per CPU: what is running there, so pick_victim() on another CPU can
 * compare. Written in running/stopping without locks; readers get a
 * possibly stale snapshot, which is fine for a heuristic.
 */
struct cpu_ctx {
	u64 running_at;	/* 0: nothing of ours running */
	u64 vtime;	/* current task's vtime when it started */
	u64 weight;	/* current task's weight (nice 0 = 100) */
	u64 run_since_sleep;	/* current task's, when it started */
	bool batch;		/* current task's class, when it started */
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, KAIROS_MAX_CPUS);
	__type(key, u32);
	__type(value, struct cpu_ctx);
} cpu_ctxs SEC(".maps");

struct kairos_timer {
	struct bpf_timer timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct kairos_timer);
} timer_map SEC(".maps");

/* One-shot timer for delayed preemption; wait_timer_armed: it is pending. */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct kairos_timer);
} wait_timer_map SEC(".maps");

static u32 wait_timer_armed;

static void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
	if (cnt_p)
		(*cnt_p)++;
}

static struct cpu_ctx *lookup_cpu_ctx(s32 cpu)
{
	u32 key = cpu;

	return bpf_map_lookup_elem(&cpu_ctxs, &key);
}

static u64 cur_slice(void)
{
	return mode == MODE_LATENCY ? lat_slice_ns : batch_slice_ns;
}

/* Integer EWMA: avg += (sample - avg) / 2^shift, without signed math. */
static u64 ewma(u64 avg, u64 sample, u32 shift)
{
	return avg - (avg >> shift) + (sample >> shift);
}

static bool is_batch(u64 run_since_sleep)
{
	return run_since_sleep >= batch_min_run_ns;
}

/* @p's class now: by policy with class_policy, else by the run-time rule. */
static bool task_batch(const struct task_struct *p, const struct task_ctx *tctx)
{
	if (class_policy)
		return p->policy == SCHED_BATCH || p->policy == SCHED_IDLE;
	return tctx && is_batch(tctx->run_since_sleep);
}

/*
 * --split: @cpu's rank among the split CPUs, -1 if it isn't one.
 * MEMBER_VPTR (scx/common.bpf.h) does the range check in inline asm next to
 * the access; a plain C check let the compiler compute the address before
 * it, and the verifier rejected the access as unbounded.
 */
static s32 split_rank_of(s32 cpu)
{
	const volatile s32 *rank;

	if (cpu < 0)
		return -1;
	rank = MEMBER_VPTR(split_rank, [cpu]);
	return rank ? *rank : -1;
}

/* --split: is @cpu an LC CPU now? CPUs outside the split count as LC. */
static bool lc_cpu(s32 cpu)
{
	s32 rank = split_rank_of(cpu);

	return rank < 0 || rank < (s32)nr_lc;
}

/* --split: slice for a task of class @batch starting on @cpu. */
static u64 split_slice(bool batch, s32 cpu)
{
	if (!batch)
		return lat_slice_ns;
	return lc_cpu(cpu) ? borrow_slice_ns : batch_slice_ns;
}

/*
 * The vtime @p is queued with. scx_simple's rule: a task that slept doesn't
 * keep all the vtime it didn't use, only up to sleep_credit_ns of it.
 */
static u64 queue_vtime(const struct task_struct *p)
{
	u64 vtime = p->scx.dsq_vtime;

	if (time_before(vtime, vtime_now - sleep_credit_ns))
		vtime = vtime_now - sleep_credit_ns;
	return vtime;
}

/*
 * LATENCY mode: pick a CPU for woken task @p (queue vtime @vtime) to
 * preempt. Among the busy CPUs @p may run on, take the one whose current
 * task has the largest vtime, counting what it has run since it started,
 * and only if that is larger than @vtime: a woken task never preempts a
 * task that is behind it in its fair share. With victim_batch_only, only
 * CPUs running a batch task count. Returns -1 if there is none; sets
 * *@batch to whether the chosen CPU's task is batch.
 */
static s32 pick_victim(struct task_struct *p, u64 vtime, bool *batch)
{
	u64 now = scx_bpf_now(), best_vtime = vtime;
	bool pinned_here = is_migration_disabled(p);
	s32 cpu, best = -1;

	bpf_for(cpu, 0, nr_cpus) {
		struct cpu_ctx *cctx;
		u64 cur, ran;
		bool b;

		if (!bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;
		/*
		 * The kernel stops the scheduler if we move a task with
		 * migration disabled to another CPU.
		 */
		if (pinned_here && cpu != scx_bpf_task_cpu(p))
			continue;
		cctx = lookup_cpu_ctx(cpu);
		if (!cctx || !cctx->running_at || !cctx->weight)
			continue;
		/* what it has run since it started on this CPU */
		ran = time_after(now, cctx->running_at) ? now - cctx->running_at : 0;
		b = class_policy ? cctx->batch : is_batch(cctx->run_since_sleep + ran);
		if (victim_batch_only && !b)
			continue;
		cur = cctx->vtime + ran * 100 / cctx->weight;
		if (time_after(cur, best_vtime)) {
			best_vtime = cur;
			best = cpu;
			*batch = b;
		}
	}
	return best;
}

/*
 * Diagnostics group of @p: the first --diag-comm prefix its name starts
 * with, else DIAG_NAMED ("other"). p->comm is copied to the stack first:
 * the verifier only allows constant offsets into a task_struct, but a
 * bounded variable index into a stack buffer.
 */
static u32 comm_group(const struct task_struct *p)
{
	char comm[DIAG_COMM_LEN];
	u32 g, i;

	__builtin_memcpy(comm, p->comm, sizeof(comm));
	for (g = 0; g < DIAG_NAMED; g++) {
		if (!diag_comm[g][0])
			break;	/* the loader fills the names from slot 0 */
		for (i = 0; i < DIAG_COMM_LEN; i++) {
			if (!diag_comm[g][i])
				return g;	/* the whole prefix matched */
			if (comm[i] != diag_comm[g][i])
				break;
		}
	}
	return DIAG_NAMED;
}

static const u64 diag_bounds_us[DIAG_BUCKETS - 1] = { DIAG_BOUNDS_US };

/* Index of the bucket for a wait of @ns: the first bound it is below. */
static u32 diag_bucket(u64 ns)
{
	u32 i;

	for (i = 0; i < DIAG_BUCKETS - 1; i++)
		if (ns < diag_bounds_us[i] * 1000)
			break;
	return i;
}

/* This CPU's counters for group @g, or NULL. */
static struct diag_group *diag_group(u32 g)
{
	u32 zero = 0;
	struct diag *d = bpf_map_lookup_elem(&diag_map, &zero);

	if (!d || g >= DIAG_GROUPS)	/* the check keeps the verifier happy */
		return NULL;
	return &d->g[g];
}

/*
 * Count a preemption of @cpu by a woken task (task context @tctx): per
 * preempted CPU, and with --diag mark the task so that stopping() records
 * how long the run it preempted for lasted.
 */
static void count_preempt(s32 cpu, struct task_ctx *tctx)
{
	u64 *cnt = MEMBER_VPTR(preempt_on, [cpu]);

	if (cnt)
		__sync_fetch_and_add(cnt, 1);
	if (diag && tctx)
		tctx->preempted_in = 1;
}

static void record_wait(u64 ns)
{
	__sync_fetch_and_add(&wait_sum_ns, ns);
	__sync_fetch_and_add(&wait_cnt, 1);
	/* Unlocked max: a concurrent larger value can be lost. Fine for a stat. */
	if (ns > wait_max_ns)
		wait_max_ns = ns;
}

/*
 * --split: claim an idle CPU for @p, or return -1. First CPUs of its own
 * pool (LC CPUs for an LC task, batch CPUs for a batch task), then of the
 * other; within a pool @prev_cpu first, for its warm cache.
 * scx_bpf_test_and_clear_cpu_idle() claims the CPU, so two wakeups can't
 * both get it.
 */
static s32 split_pick_idle(struct task_struct *p, s32 prev_cpu, bool batch)
{
	u32 pass;
	s32 cpu;

	for (pass = 0; pass < 2; pass++) {
		bool want_lc = (pass == 0) != batch;

		if (prev_cpu >= 0 && bpf_cpumask_test_cpu(prev_cpu, p->cpus_ptr) &&
		    lc_cpu(prev_cpu) == want_lc && scx_bpf_test_and_clear_cpu_idle(prev_cpu))
			return prev_cpu;
		bpf_for(cpu, 0, nr_cpus) {
			if (cpu == prev_cpu || !bpf_cpumask_test_cpu(cpu, p->cpus_ptr) ||
			    lc_cpu(cpu) != want_lc)
				continue;
			if (scx_bpf_test_and_clear_cpu_idle(cpu))
				return cpu;
		}
	}
	return -1;
}

/*
 * --split: a CPU to preempt for woken LC task @p: an LC CPU running a
 * borrowed batch task, chosen by borrower_pick (@prev: @p's previous
 * CPU). If @p can't run on any LC CPU (e.g. a per-CPU kernel thread on a
 * batch CPU), any allowed CPU running a batch task. Returns -1 if there
 * is none. @p may be NULL (the delayed-preemption timer): then any LC CPU.
 */
static s32 split_pick_borrower(struct task_struct *p, s32 prev)
{
	bool pinned_here = p && is_migration_disabled(p), can_lc = false;
	s32 cpu, any = -1, best = -1;
	u64 best_at = 0;

	bpf_for(cpu, 0, nr_cpus) {
		struct cpu_ctx *cctx;
		bool lc;

		if (p && !bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;
		if (pinned_here && cpu != prev)
			continue;
		lc = lc_cpu(cpu);
		can_lc |= lc;
		cctx = lookup_cpu_ctx(cpu);
		if (!cctx || !cctx->running_at || !cctx->batch)
			continue;
		if (!lc) {
			if (any < 0)
				any = cpu;
			continue;
		}
		if (borrower_pick == PICK_FIRST || (borrower_pick == PICK_PREV && cpu == prev))
			return cpu;
		if (best < 0 || (borrower_pick == PICK_LONGEST &&
				 time_before(cctx->running_at, best_at))) {
			best = cpu;
			best_at = cctx->running_at;
		}
	}
	if (best >= 0)
		return best;
	return can_lc ? -1 : any;
}

/* Can idle CPU @cpu be claimed for @p (of class @batch), in @p's pool? */
static bool claim_in_pool(struct task_struct *p, s32 cpu, bool batch)
{
	return cpu >= 0 && bpf_cpumask_test_cpu(cpu, p->cpus_ptr) && lc_cpu(cpu) != batch &&
	       scx_bpf_test_and_clear_cpu_idle(cpu);
}

/*
 * --wake-sync: may @p (of class @batch), woken by a sync wakeup, run on
 * the waker's CPU @cpu? scx_bpf_select_cpu_dfl()'s rule
 * (kernel/sched/ext_idle.c): if @p may run there, nothing is queued
 * there, the waker isn't exiting and some CPU is idle (the system isn't
 * saturated). Here also only if @cpu is in @p's pool and runs a task of
 * @p's class, so an LC task doesn't queue behind a borrower.
 */
static bool sync_ok(struct task_struct *p, s32 cpu, bool batch)
{
	struct task_struct *waker = bpf_get_current_task_btf();
	const struct cpumask *idle;
	struct cpu_ctx *cctx;
	bool any_idle;

	if (!bpf_cpumask_test_cpu(cpu, p->cpus_ptr) || lc_cpu(cpu) == batch ||
	    (waker->flags & PF_EXITING) || scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu))
		return false;
	cctx = lookup_cpu_ctx(cpu);
	if (!cctx || !cctx->running_at || cctx->batch != batch)
		return false;
	idle = scx_bpf_get_idle_cpumask();
	any_idle = !bpf_cpumask_empty(idle);
	scx_bpf_put_idle_cpumask(idle);
	return any_idle;
}

static s32 split_select_cpu(struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	bool batch = task_batch(p, tctx);
	s32 cpu;

	/*
	 * Sync wakeup: as the kernel, @prev_cpu if it is idle, else the
	 * waker's CPU (busy, so not claimed: @p runs there once the waker
	 * sleeps), before looking for other idle CPUs. The kernel also
	 * requires @prev_cpu to share a cache with the waker; all CPUs here
	 * do.
	 */
	if (wake_sync && (wake_flags & SCX_WAKE_SYNC)) {
		if (claim_in_pool(p, prev_cpu, batch)) {
			cpu = prev_cpu;
			goto local;
		}
		cpu = bpf_get_smp_processor_id();
		if (sync_ok(p, cpu, batch)) {
			stat_inc(STAT_SYNC);
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, split_slice(batch, cpu), 0);
			return cpu;
		}
	}
	cpu = split_pick_idle(p, prev_cpu, batch);
	if (cpu < 0)
		return prev_cpu;	/* enqueue() decides */
local:
	stat_inc(STAT_LOCAL);
	scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, split_slice(batch, cpu), 0);
	return cpu;
}

/*
 * After inserting @p into a shared queue: wake an idle CPU @p may run on,
 * which then pulls work in dispatch(). scx_bpf_pick_idle_cpu() also claims
 * the CPU (clears its idle bit), so two inserts don't both kick the same
 * one and select_cpu() won't place a woken task on it meanwhile; the bit
 * is set again when the CPU next goes idle.
 */
static void kick_an_idle_cpu(const struct task_struct *p)
{
	s32 cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);

	if (cpu >= 0) {
		stat_inc(STAT_KICK);
		scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
	}
}

static void kick_idle_cpu(const struct task_struct *p)
{
	if (kick_idle)
		kick_an_idle_cpu(p);
}

/* --split: does woken LC task (context @tctx) preempt a borrower at once? */
static bool preempt_at_once(const struct task_ctx *tctx)
{
	if (!preempt_wait_ns)
		return true;
	return preempt_min_run_ns && tctx && tctx->avg_run >= preempt_min_run_ns;
}

/*
 * Arm the delayed-preemption timer unless it is pending. Only the first
 * delayed task arms it; wait_timerfn() re-arms it for later ones.
 */
static void arm_wait_timer(void)
{
	struct kairos_timer *t;
	u32 key = 0;

	if (__sync_val_compare_and_swap(&wait_timer_armed, 0, 1))
		return;
	t = bpf_map_lookup_elem(&wait_timer_map, &key);
	if (t)
		bpf_timer_start(&t->timer, preempt_wait_ns, 0);
}

static void split_enqueue(struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	bool batch = task_batch(p, tctx);
	u64 vtime = queue_vtime(p);

	if (!batch && borrow_preempt && (enq_flags & SCX_ENQ_WAKEUP)) {
		s32 cpu = split_pick_borrower(p, scx_bpf_task_cpu(p));

		if (cpu >= 0 && preempt_at_once(tctx)) {
			stat_inc(STAT_PREEMPT);
			stat_inc(STAT_PREEMPT_BATCH);
			count_preempt(cpu, tctx);
			p->scx.dsq_vtime = vtime;
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, lat_slice_ns,
					   enq_flags | SCX_ENQ_PREEMPT);
			return;
		}
		if (cpu >= 0) {
			/*
			 * Delayed: wait in LC_DSQ like a task with nothing to
			 * preempt, but with the timer as a deadline. A CPU may
			 * have gone idle since select_cpu() found none; wake it.
			 */
			stat_inc(STAT_DELAYED);
			stat_inc(STAT_GLOBAL);
			if (tctx)
				tctx->delayed = 1;
			scx_bpf_dsq_insert_vtime(p, LC_DSQ, lat_slice_ns, vtime, enq_flags);
			kick_an_idle_cpu(p);
			arm_wait_timer();
			return;
		}
		stat_inc(STAT_NO_VICTIM);
	}
	stat_inc(STAT_GLOBAL);
	if (!batch && lc_percpu) {
		s32 cpu = scx_bpf_task_cpu(p);

		scx_bpf_dsq_insert_vtime(p, LC_PCPU_DSQ(cpu), lat_slice_ns, vtime, enq_flags);
		/* only @cpu looks at this queue soon; wake it if it went idle */
		scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
		return;
	}
	scx_bpf_dsq_insert_vtime(p, batch ? BATCH_DSQ : LC_DSQ,
				 batch ? batch_slice_ns : lat_slice_ns, vtime, enq_flags);
	kick_idle_cpu(p);
}

s32 BPF_STRUCT_OPS(kairos_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	if (split)
		return split_select_cpu(p, prev_cpu, wake_flags);
	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		stat_inc(STAT_LOCAL);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, cur_slice(), 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(kairos_enqueue, struct task_struct *p, u64 enq_flags)
{
	u64 vtime;

	/* Debug output for trace_pipe; only with the loader's --trace. */
	if (trace_enqueue)
		bpf_printk("kairos enqueue: pid=%d comm=%s enq_flags=0x%llx",
			   p->pid, p->comm, enq_flags);

	if (split) {
		split_enqueue(p, enq_flags);
		return;
	}
	vtime = queue_vtime(p);

	/*
	 * A wakeup that reaches enqueue found no idle CPU in select_cpu (or
	 * the task can only run on one CPU). In LATENCY mode it preempts a
	 * busy CPU: inserting into that CPU's local queue with
	 * SCX_ENQ_PREEMPT puts it first in line there and sets the current
	 * task's slice to 0, so that task stops and goes back through
	 * enqueue (with enq_flags 0) into the shared queue.
	 */
	if (mode == MODE_LATENCY && (enq_flags & SCX_ENQ_WAKEUP)) {
		bool batch = false;
		s32 cpu = pick_victim(p, vtime, &batch);

		if (cpu >= 0) {
			stat_inc(STAT_PREEMPT);
			if (batch)
				stat_inc(STAT_PREEMPT_BATCH);
			count_preempt(cpu, bpf_task_storage_get(&task_ctxs, p, 0, 0));
			/* the vtime insert below would apply the clamp; do it here */
			p->scx.dsq_vtime = vtime;
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, cur_slice(),
					   enq_flags | SCX_ENQ_PREEMPT);
			return;
		}
		stat_inc(STAT_NO_VICTIM);
	}

	stat_inc(STAT_GLOBAL);
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, cur_slice(), vtime, enq_flags);
	kick_idle_cpu(p);
}

void BPF_STRUCT_OPS(kairos_dispatch, s32 cpu, struct task_struct *prev)
{
	s32 c;

	if (!split) {
		scx_bpf_dsq_move_to_local(SHARED_DSQ);
		return;
	}
	if (lc_percpu) {
		/*
		 * Own LC queue first on an LC CPU (after batch on a batch
		 * CPU); another CPU's LC queue only when there is nothing
		 * else to run.
		 */
		bool lc = lc_cpu(cpu);

		if (lc && scx_bpf_dsq_move_to_local(LC_PCPU_DSQ(cpu)))
			return;
		if (scx_bpf_dsq_move_to_local(BATCH_DSQ))
			return;
		if (!lc && scx_bpf_dsq_move_to_local(LC_PCPU_DSQ(cpu)))
			return;
		bpf_for(c, 0, nr_cpus) {
			if (c != cpu && scx_bpf_dsq_move_to_local(LC_PCPU_DSQ(c)))
				return;
		}
		return;
	}
	/* own pool's queue first, then lend the CPU to the other pool */
	if (lc_cpu(cpu)) {
		if (!scx_bpf_dsq_move_to_local(LC_DSQ))
			scx_bpf_dsq_move_to_local(BATCH_DSQ);
	} else {
		if (!scx_bpf_dsq_move_to_local(BATCH_DSQ))
			scx_bpf_dsq_move_to_local(LC_DSQ);
	}
}

/*
 * runnable/quiescent bracket the time a task is runnable (queued or
 * running), so their difference is the number of runnable tasks. The
 * kernel skips both when it only moves a task between CPUs.
 */
void BPF_STRUCT_OPS(kairos_runnable, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;

	__sync_fetch_and_add(&nr_runnable, 1);
	if (enq_flags & SCX_ENQ_WAKEUP)
		__sync_fetch_and_add(&nr_wakeups, 1);

	tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	if (tctx) {
		tctx->ready_at = scx_bpf_now();
		tctx->ready_kind = enq_flags & SCX_ENQ_WAKEUP ? READY_WAKEUP : READY_OTHER;
	}
}

void BPF_STRUCT_OPS(kairos_quiescent, struct task_struct *p, u64 deq_flags)
{
	__sync_fetch_and_sub(&nr_runnable, 1);
}

void BPF_STRUCT_OPS(kairos_running, struct task_struct *p)
{
	/* running() may be called from another CPU; use the task's CPU */
	s32 cpu = scx_bpf_task_cpu(p);
	u64 now = scx_bpf_now();
	struct task_ctx *tctx;
	struct cpu_ctx *cctx;

	/*
	 * Global vtime always progresses forward as tasks start executing. The
	 * test and update can be performed concurrently from multiple CPUs and
	 * thus racy. Any error should be contained and temporary. Let's just
	 * live with it. (scx_simple)
	 */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;

	tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	if (tctx) {
		struct diag_group *dg = NULL;

		if (diag) {
			tctx->group = comm_group(p);
			dg = diag_group(tctx->group);
			if (dg)
				dg->runs++;
		}
		/* scx_bpf_now() may differ slightly between CPUs; skip if negative */
		if (tctx->ready_at && time_after(now, tctx->ready_at)) {
			u64 wait = now - tctx->ready_at;
			u32 kind = tctx->ready_kind;

			record_wait(wait);
			if (kind == READY_WAKEUP && cpu < 64 && (signal_cpus >> cpu) & 1) {
				__sync_fetch_and_add(&lat_wakeups, 1);
				if (wait > lat_target_ns)
					__sync_fetch_and_add(&lat_late, 1);
			}
			if (dg && kind < KAIROS_NR_READY) {
				dg->waits[kind][diag_bucket(wait)]++;
				dg->wait_ns[kind] += wait;
			}
		}
		tctx->ready_at = 0;
		tctx->delayed = 0;
		tctx->running_at = now;
		tctx->slice_at_run = p->scx.slice;
	}

	cctx = lookup_cpu_ctx(cpu);
	if (cctx) {
		cctx->vtime = p->scx.dsq_vtime;
		cctx->weight = p->scx.weight;
		cctx->run_since_sleep = tctx ? tctx->run_since_sleep : 0;
		cctx->batch = task_batch(p, tctx);
		cctx->running_at = now;
	}
	/*
	 * --split: a batch task taken from the batch queue by an LC CPU got a
	 * batch slice when it was queued; on an LC CPU it only borrows, so
	 * shorten it. (It can still be preempted by an LC wakeup sooner.)
	 */
	if (split && task_batch(p, tctx) && lc_cpu(cpu)) {
		stat_inc(STAT_BORROW);
		if (p->scx.slice > borrow_slice_ns)
			scx_bpf_task_set_slice(p, borrow_slice_ns);
	}
}

void BPF_STRUCT_OPS(kairos_stopping, struct task_struct *p, bool runnable)
{
	u64 now = scx_bpf_now();
	struct task_ctx *tctx;
	struct cpu_ctx *cctx;
	bool was_batch;
	u64 ran = 0;

	tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	if (tctx) {
		/* Charge the time it ran, scaled by the inverse of its weight. */
		if (tctx->running_at && time_after(now, tctx->running_at))
			ran = now - tctx->running_at;
		p->scx.dsq_vtime += ran * 100 / p->scx.weight;
		tctx->running_at = 0;
		/*
		 * Still runnable (slice used up or preempted): waiting again.
		 * The slice counts down with the time the task runs, so it ran
		 * out if the task ran at least as long as the slice it started
		 * with. Shorter means it was preempted: by us (SCX_ENQ_PREEMPT
		 * sets its slice to 0) or by a higher scheduling class. A
		 * slice the kernel refilled while the task kept running counts
		 * as ran out.
		 */
		if (runnable) {
			tctx->ready_at = now;
			tctx->ready_kind = ran >= tctx->slice_at_run ?
					   READY_SLICE_END : READY_PREEMPTED;
		}
		if (diag) {
			struct diag_group *dg = diag_group(tctx->group);

			if (dg) {
				dg->run_ns += ran;
				if (tctx->preempted_in) {
					dg->preempt_runs[diag_bucket(ran)]++;
					dg->preempt_run_ns += ran;
				}
			}
			tctx->preempted_in = 0;
		}
		/* LC demand for --split: CPU time of LC tasks on split CPUs */
		if (split && !task_batch(p, tctx)) {
			s32 c = scx_bpf_task_cpu(p);

			if (split_rank_of(c) >= 0)
				__sync_fetch_and_add(&lc_run_ns, ran);
		}
		/*
		 * Classification. The kernel calls stopping(p, false) when the
		 * running task is dequeued (it blocked or went to sleep), which
		 * restarts the count, and stopping(p, true) when it is put back
		 * while still runnable (slice expired or preempted).
		 */
		was_batch = is_batch(tctx->run_since_sleep);
		if (!runnable)	/* the run since it woke is over */
			tctx->avg_run = ewma(tctx->avg_run, tctx->run_since_sleep + ran,
					     avg_run_shift);
		tctx->run_since_sleep = runnable ? tctx->run_since_sleep + ran : 0;
		if (is_batch(tctx->run_since_sleep) != was_batch) {
			stat_inc(was_batch ? STAT_FROM_BATCH : STAT_TO_BATCH);
			if (trace_class)
				bpf_printk("kairos class: pid=%d comm=%s batch=%d",
					   p->pid, p->comm, !was_batch);
		}
	}

	cctx = lookup_cpu_ctx(scx_bpf_task_cpu(p));
	if (cctx)
		cctx->running_at = 0;
}

void BPF_STRUCT_OPS(kairos_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

s32 BPF_STRUCT_OPS(kairos_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	/* zero-filled: not batch until it has run batch_min_run_ns */
	if (!bpf_task_storage_get(&task_ctxs, p, 0, BPF_LOCAL_STORAGE_GET_F_CREATE))
		return -ENOMEM;
	return 0;
}

static u64 last_tick_at, last_wakeups, last_wait_sum, last_wait_cnt;
static u64 last_lat_wakeups, last_lat_late, last_lc_run_ns;

/*
 * Verifier: a sum over a bpf_for loop gets a wider range in every
 * iteration, so the loop's states never match and it isn't proven to end
 * ("jumps too complex"). Masking the sum in the loop keeps its range
 * fixed; far fewer than 2^24 tasks are ever queued.
 */
#define QUEUED_MASK 0xffffff

/* --split: LC tasks waiting in our queues (LC_DSQ, or the per-CPU ones). */
static u64 nr_lc_queued(void)
{
	u64 n = scx_bpf_dsq_nr_queued(LC_DSQ) & QUEUED_MASK;
	s32 cpu;

	if (lc_percpu)
		bpf_for(cpu, 0, nr_cpus)
			n = (n + scx_bpf_dsq_nr_queued(LC_PCPU_DSQ(cpu))) & QUEUED_MASK;
	return n;
}

/*
 * --split: size the LC pool. Demand per tick = CPUs' worth of LC run time
 * in the tick + one CPU per LC task waiting in the LC queue (work that
 * didn't get a CPU), averaged with weight 1/2^lc_shift per tick (~64 ms):
 * millisecond bursts are absorbed by idle CPUs and by preempting
 * borrowers, so the pool only needs to follow slower changes. (An ~8 ms
 * average changed the pool 11-24 times a second on an idle system.)
 * nr_lc = ceil(demand + lc_buffer), within [lc_min, lc_max]; it shrinks
 * only if it would also with lc_hyst more demand, so demand near a whole
 * CPU doesn't flip it back and forth. Tasks keep running where they are
 * when a CPU changes pool; the change applies at their next dispatch.
 */
static void update_split(u64 dt)
{
	u64 run = lc_run_ns, queued = nr_lc_queued(), sample, base, want;
	u64 stamp = ml_stamp;
	u32 n;

	sample = (run - last_lc_run_ns) * 1024 / dt + queued * 1024;
	last_lc_run_ns = run;
	lc_queued_ticks += queued;
	lc_demand = ewma(lc_demand, sample, lc_shift);
	base = lc_demand;
	/* A fresh prediction may raise the demand; the loader's write is racy
	 * (two words), which can only pair a demand with a nearby stamp. */
	if (ml_ttl_ticks && stamp && nr_timer_ticks - stamp <= ml_ttl_ticks) {
		u64 pred = ml_demand;

		ml_fresh_ticks++;
		if (pred > base) {
			base = pred;
			ml_raised_ticks++;
		}
	}
	want = base + lc_buffer;
	n = (want + 1023) / 1024;
	if (n < nr_lc) {
		u32 n_hyst = (want + lc_hyst + 1023) / 1024;

		n = n_hyst < nr_lc ? n_hyst : nr_lc;
	}
	if (n < lc_min)
		n = lc_min;
	if (n > lc_max)
		n = lc_max;
	if (n != nr_lc) {
		nr_lc = n;
		nr_lc_changes++;
	}
	lc_cpus_ns += nr_lc * dt;
}

/*
 * The number of tasks in queue @dsq_id that may run on @cpu. (A loop over
 * CPUs inside this loop over the queue was too complex for the verifier.)
 */
static u32 nr_fitting(u64 dsq_id, s32 cpu)
{
	struct task_struct *p;
	u32 n = 0;

	bpf_for_each(scx_dsq, p, dsq_id, 0) {
		if (bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			n++;
	}
	return n;
}

/* --lc-queue percpu: tasks waiting in the split CPUs' own LC queues. */
static u32 nr_split_pcpu_queued(void)
{
	u32 n = 0;
	s32 cpu;

	bpf_for(cpu, 0, nr_cpus) {
		if (split_rank_of(cpu) >= 0)	/* see QUEUED_MASK */
			n = (n + scx_bpf_dsq_nr_queued(LC_PCPU_DSQ(cpu))) & QUEUED_MASK;
	}
	return n;
}

/*
 * Each tick, for the idle-time question in RESEARCH_LOG section 9: how many
 * split CPUs are idle, and how many of them could be running a task that
 * waits in our queues instead (at most one task per idle CPU). Whether a
 * task may run there is checked against the first idle split CPU only,
 * which is exact when, as in our runs, the tasks that may run on one split
 * CPU may run on all (the server and batch jobs on 0-2, the load generator
 * only on 3). Sampling keeps the idle path itself untouched; over a minute
 * it is ~60,000 samples. A CPU just claimed by select_cpu() counts as busy.
 */
static void sample_idle(u64 dt)
{
	const struct cpumask *idle = scx_bpf_get_idle_cpumask();
	u32 nr_idle = 0, nr_waiting;
	s32 cpu, first = -1;

	bpf_for(cpu, 0, nr_cpus) {
		if (split_rank_of(cpu) >= 0 && bpf_cpumask_test_cpu(cpu, idle)) {
			if (first < 0)
				first = cpu;
			nr_idle++;
		}
	}
	scx_bpf_put_idle_cpumask(idle);
	if (first < 0)
		return;
	idle_cpus_ns += nr_idle * dt;
	if (split && lc_percpu)	/* the split CPUs' own queues: their tasks fit */
		nr_waiting = nr_split_pcpu_queued() + nr_fitting(BATCH_DSQ, first);
	else if (split)
		nr_waiting = nr_fitting(LC_DSQ, first) + nr_fitting(BATCH_DSQ, first);
	else
		nr_waiting = nr_fitting(SHARED_DSQ, first);
	idle_queued_ns += (nr_waiting < nr_idle ? nr_waiting : nr_idle) * dt;
}

static void set_mode(u32 m, u64 now)
{
	mode = m;
	mode_since = now;
	nr_mode_switches++;
}

/*
 * Adaptive switching against the latency target. Fast exit: in BATCH mode,
 * too many late wakeups switch to LATENCY on the next tick. Cautious entry:
 * LATENCY mode tries BATCH again only after hold_ns, and only if wakeups
 * are on time. If BATCH then fails within hold_ns, the hold doubles (up to
 * hold_max_ns), so a steadily loaded system doesn't keep probing; a BATCH
 * stint that lasts resets it.
 */
static void update_mode(u64 now)
{
	if (!mode_since)
		mode_since = now;
	if (mode == MODE_BATCH) {
		if (late_frac > late_max) {
			if (now - mode_since < hold_ns)
				hold_ns = hold_ns * 2 < hold_max_ns ? hold_ns * 2 : hold_max_ns;
			else
				hold_ns = hold_min_ns;
			set_mode(MODE_LATENCY, now);
		}
	} else if (now - mode_since >= hold_ns && late_frac <= late_max / 2) {
		set_mode(MODE_BATCH, now);
	}
}

/*
 * Every timer_interval_ns: sample the counters and update the EWMAs. Runs
 * in softirq context on one CPU, so its own variables need no atomics.
 */
static int kairos_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	u64 now = scx_bpf_now();
	s64 nr = nr_runnable;
	u64 load = nr > 0 ? (u64)nr * 1024 / nr_cpus : 0;

	fast_load = ewma(fast_load, load, fast_shift);
	slow_load = ewma(slow_load, load, slow_shift);

	if (last_tick_at && time_after(now, last_tick_at)) {
		u64 dt = now - last_tick_at, w = nr_wakeups;
		u64 sum = wait_sum_ns, cnt = wait_cnt;
		u64 lw = lat_wakeups, ll = lat_late;

		mode_ns[mode & 1] += dt;
		if (lw > last_lat_wakeups)
			late_frac = ewma(late_frac, (ll - last_lat_late) * LATE_ONE /
					 (lw - last_lat_wakeups), late_shift);
		last_lat_wakeups = lw;
		last_lat_late = ll;
		if (adaptive)
			update_mode(now);
		if (split)
			update_split(dt);
		sample_idle(dt);

		wake_rate = ewma(wake_rate, (w - last_wakeups) * 1000000000 / dt, fast_shift);
		if (cnt > last_wait_cnt)
			wait_avg_ns = ewma(wait_avg_ns, (sum - last_wait_sum) /
					   (cnt - last_wait_cnt), fast_shift);
		last_wakeups = w;
		last_wait_sum = sum;
		last_wait_cnt = cnt;
	}
	last_tick_at = now;
	nr_timer_ticks++;

	bpf_timer_start(timer, timer_interval_ns, 0);
	return 0;
}

/*
 * Delayed preemption (preempt_wait_ns). Each firing takes the first
 * delayed LC task that has waited in LC_DSQ for preempt_wait_ns since
 * waking and preempts a borrower for it (SCX_KICK_PREEMPT: that CPU stops
 * its task and runs dispatch(), which takes LC_DSQ first on an LC CPU).
 * The task's delayed flag is cleared, so it causes at most one late
 * preemption; the kicked CPU takes the first LC task by vtime, which may
 * be another one. If more tasks are overdue, the timer fires again at
 * once; if some haven't waited that long yet, it is re-armed for the
 * first of them. Only tasks that found a borrower they may run on when
 * they were queued are delayed, so the load generator's tasks, which
 * wait in LC_DSQ too but may only run on CPU 3, never count.
 *
 * Verifier: an exact count of overdue tasks, used to kick that many CPUs,
 * made the loops too complex (each count value is a separate state), so
 * the state here is two booleans and the time to the next deadline.
 */
static int wait_timerfn(void *map, int *key, struct bpf_timer *timer)
{
	u64 now = scx_bpf_now(), next = 0;
	bool found = false, more = false, kicked = false;
	struct task_struct *p;
	s32 cpu, prev = -1;

	wait_timer_armed = 0;
	bpf_for_each(scx_dsq, p, LC_DSQ, 0) {
		struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
		u64 waited;

		if (!tctx || !tctx->delayed || !tctx->ready_at)
			continue;
		waited = time_after(now, tctx->ready_at) ? now - tctx->ready_at : 0;
		if (waited < preempt_wait_ns) {
			if (!next || preempt_wait_ns - waited < next)
				next = preempt_wait_ns - waited;
		} else if (!found) {
			tctx->delayed = 0;
			prev = scx_bpf_task_cpu(p);
			found = true;
		} else {
			more = true;
		}
	}
	if (found) {
		cpu = split_pick_borrower(NULL, prev);
		if (cpu >= 0) {
			scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
			stat_inc(STAT_LATE_PREEMPT);
			count_preempt(cpu, NULL);
			kicked = true;
		}
	}
	/* more overdue tasks and a borrower left to kick: again at once */
	if (more && kicked)
		next = 1;
	if (next && !__sync_val_compare_and_swap(&wait_timer_armed, 0, 1))
		bpf_timer_start(timer, next, 0);
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(kairos_init)
{
	struct bpf_timer *timer;
	u32 key = 0;
	int ret;

	nr_cpus = scx_bpf_nr_cpu_ids();
	if (nr_cpus > KAIROS_MAX_CPUS) {
		scx_bpf_error("%u CPUs, at most %d supported", nr_cpus, KAIROS_MAX_CPUS);
		return -E2BIG;
	}
	/* adaptive starts in LATENCY mode, the safe one for latency */
	mode = adaptive ? MODE_LATENCY : forced_mode;
	hold_ns = hold_min_ns;
	nr_lc = lc_max;	/* start with every split CPU for LC; the timer shrinks it */

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (!ret)
		ret = scx_bpf_create_dsq(LC_DSQ, -1);
	if (!ret)
		ret = scx_bpf_create_dsq(BATCH_DSQ, -1);
	if (!ret && lc_percpu) {
		s32 cpu;

		bpf_for(cpu, 0, nr_cpus) {
			ret = scx_bpf_create_dsq(LC_PCPU_DSQ(cpu), -1);
			if (ret)
				break;
		}
	}
	if (ret) {
		scx_bpf_error("failed to create DSQs (%d)", ret);
		return ret;
	}

	timer = bpf_map_lookup_elem(&timer_map, &key);
	if (!timer)
		return -ESRCH;
	bpf_timer_init(timer, &timer_map, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, kairos_timerfn);
	ret = bpf_timer_start(timer, timer_interval_ns, 0);
	if (ret) {
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
		return ret;
	}

	/* started only by arm_wait_timer() */
	timer = bpf_map_lookup_elem(&wait_timer_map, &key);
	if (!timer)
		return -ESRCH;
	bpf_timer_init(timer, &wait_timer_map, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, wait_timerfn);
	return 0;
}

void BPF_STRUCT_OPS(kairos_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(kairos_ops,
	       .select_cpu		= (void *)kairos_select_cpu,
	       .enqueue			= (void *)kairos_enqueue,
	       .dispatch		= (void *)kairos_dispatch,
	       .runnable		= (void *)kairos_runnable,
	       .running			= (void *)kairos_running,
	       .stopping		= (void *)kairos_stopping,
	       .quiescent		= (void *)kairos_quiescent,
	       .enable			= (void *)kairos_enable,
	       .init_task		= (void *)kairos_init_task,
	       .init			= (void *)kairos_init,
	       .exit			= (void *)kairos_exit,
	       .name			= "kairos");
