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
 * late share of wakeups, x1024. Only tasks on signal_cpus count (bit i =
 * CPU i), so e.g. a load generator on its own CPU doesn't drive the mode.
 */
const volatile bool adaptive;
const volatile u64 lat_target_ns = 1000000;
const volatile u64 late_max = 51;		/* x1024, i.e. 5% */
const volatile u64 hold_min_ns = 200000000;	/* LATENCY at least this long */
const volatile u64 hold_max_ns = 5000000000;
const volatile u64 signal_cpus = ~0ULL;

UEI_DEFINE(uei);

#define SHARED_DSQ 0

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
u64 late_frac;			/* EWMA of late share, x1024 */
u64 nr_mode_switches;
u64 mode_ns[2];			/* time spent in each mode */
u64 hold_ns;			/* current LATENCY hold before trying BATCH */
static u64 mode_since;

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, KAIROS_NR_STATS);
} stats SEC(".maps");

/* Per task: timestamps for wait time and run-time charging; class. */
struct task_ctx {
	u64 ready_at;	/* when it last became ready to run; 0: not waiting */
	u64 running_at;	/* when it last started running; 0: not running */
	u64 run_since_sleep;	/* CPU time run since it last slept */
	bool woken;		/* waiting after a wakeup (not a requeue) */
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
		b = is_batch(cctx->run_since_sleep + ran);
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

static void record_wait(u64 ns)
{
	__sync_fetch_and_add(&wait_sum_ns, ns);
	__sync_fetch_and_add(&wait_cnt, 1);
	/* Unlocked max: a concurrent larger value can be lost. Fine for a stat. */
	if (ns > wait_max_ns)
		wait_max_ns = ns;
}

s32 BPF_STRUCT_OPS(kairos_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

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
}

void BPF_STRUCT_OPS(kairos_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
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
		tctx->woken = enq_flags & SCX_ENQ_WAKEUP;
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
		/* scx_bpf_now() may differ slightly between CPUs; skip if negative */
		if (tctx->ready_at && time_after(now, tctx->ready_at)) {
			u64 wait = now - tctx->ready_at;

			record_wait(wait);
			if (tctx->woken && cpu < 64 && (signal_cpus >> cpu) & 1) {
				__sync_fetch_and_add(&lat_wakeups, 1);
				if (wait > lat_target_ns)
					__sync_fetch_and_add(&lat_late, 1);
			}
		}
		tctx->ready_at = 0;
		tctx->woken = false;
		tctx->running_at = now;
	}

	cctx = lookup_cpu_ctx(cpu);
	if (cctx) {
		cctx->vtime = p->scx.dsq_vtime;
		cctx->weight = p->scx.weight;
		cctx->run_since_sleep = tctx ? tctx->run_since_sleep : 0;
		cctx->running_at = now;
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
		/* Still runnable (slice used up or preempted): waiting again. */
		if (runnable)
			tctx->ready_at = now;
		/*
		 * Classification. The kernel calls stopping(p, false) when the
		 * running task is dequeued (it blocked or went to sleep), which
		 * restarts the count, and stopping(p, true) when it is put back
		 * while still runnable (slice expired or preempted).
		 */
		was_batch = is_batch(tctx->run_since_sleep);
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
static u64 last_lat_wakeups, last_lat_late;

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
			late_frac = ewma(late_frac, (ll - last_lat_late) * 1024 /
					 (lw - last_lat_wakeups), fast_shift);
		last_lat_wakeups = lw;
		last_lat_late = ll;
		if (adaptive)
			update_mode(now);

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

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) {
		scx_bpf_error("failed to create DSQ %d (%d)", SHARED_DSQ, ret);
		return ret;
	}

	timer = bpf_map_lookup_elem(&timer_map, &key);
	if (!timer)
		return -ESRCH;
	bpf_timer_init(timer, &timer_map, CLOCK_MONOTONIC);
	bpf_timer_set_callback(timer, kairos_timerfn);
	ret = bpf_timer_start(timer, timer_interval_ns, 0);
	if (ret)
		scx_bpf_error("bpf_timer_start failed (%d)", ret);
	return ret;
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
