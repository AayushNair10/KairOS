/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_kairos loader: sets the tunables, loads and attaches the BPF policy,
 * then prints its statistics once a second until Ctrl-C (SIGINT/SIGTERM).
 *
 * Based on scx_simple's loader: Copyright (c) 2022 Meta Platforms, Inc. and
 * affiliates; Tejun Heo <tj@kernel.org>; David Vernet <dvernet@meta.com>.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <getopt.h>
#include <libgen.h>
#include <time.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_kairos.h"
#include "scx_kairos.bpf.skel.h"

const char help_fmt[] =
"KairOS: load-adaptive wakeup preemption.\n"
"\n"
"See the top-level comment in scx_kairos.bpf.c for more details.\n"
"\n"
"Usage: %s [options]\n"
"\n"
"  -n, --never-preempt     BATCH mode: woken tasks never preempt (default)\n"
"  -p, --force-preempt     LATENCY mode: woken tasks preempt a busy CPU\n"
"  -a, --adaptive          switch between the modes against a latency target:\n"
"      --lat-target-us N   a woken task that waits longer is late (default 1000)\n"
"      --late-pct N        BATCH -> LATENCY above N%% late wakeups (default 5)\n"
"      --late-shift N      late share EWMA weight 1/2^N per tick (default 6)\n"
"      --hold-ms N         stay in LATENCY at least N ms before trying BATCH\n"
"                          again (default 200; doubles after a failed try)\n"
"      --hold-max-ms N     longest hold (default 5000)\n"
"      --signal-cpus LIST  only wakeups on these CPUs count, e.g. 0-2\n"
"                          (default all)\n"
"      --lat-slice-us N    slice in LATENCY mode (default 3000)\n"
"      --batch-slice-us N  slice in BATCH mode (default 20000)\n"
"      --sleep-credit-us N vtime credit kept by a sleeping task (default 20000)\n"
"      --timer-us N        load-signal timer period (default 1000)\n"
"      --fast-shift N      fast EWMA weight 1/2^N per timer tick (default 3)\n"
"      --slow-shift N      slow EWMA weight 1/2^N per timer tick (default 6)\n"
"      --split             soft CPU split instead of the modes: LC CPUs and\n"
"                          batch CPUs, each pool serving its own queue first\n"
"      --split-cpus LIST   CPUs the split manages (default all)\n"
"      --lc-min N          fewest LC CPUs (default 1)\n"
"      --lc-max N          most LC CPUs (default: all split CPUs)\n"
"      --lc-buffer-pct N   spare LC capacity on top of LC demand, in %% of a\n"
"                          CPU (default 50)\n"
"      --lc-shift N        LC demand EWMA weight 1/2^N per tick (default 6)\n"
"      --lc-hyst-pct N     shrink the LC pool only if it would with N%% of a\n"
"                          CPU more demand (default 25)\n"
"      --borrow-slice-us N slice of a batch task on an LC CPU (default 3000)\n"
"      --no-borrow-preempt a woken LC task doesn't preempt a batch task on an\n"
"                          LC CPU; it waits for the borrow slice instead\n"
"      --borrower-pick first|prev|longest  which borrower a woken LC task\n"
"                          preempts: the first LC CPU running one (CPU 0\n"
"                          first), its previous CPU if that runs one, or the\n"
"                          one running longest (default first)\n"
"      --claim-borrower    --split: one preemption per borrower at a time; a\n"
"                          second LC wakeup takes another borrower or queues\n"
"                          (off by default)\n"
"      --lc-queue shared|percpu  queued LC tasks wait in one queue that\n"
"                          every LC CPU serves first (shared), or in their\n"
"                          CPU's own queue, which other CPUs serve only when\n"
"                          idle (percpu; an ablation) (default shared)\n"
"      --preempt-wait-us N a woken LC task that finds no idle CPU waits up to\n"
"                          N us for an LC CPU to free up before it preempts a\n"
"                          borrower (default 0: preempts at once)\n"
"      --preempt-min-run-us N  with --preempt-wait-us, a task whose runs (waking\n"
"                          to sleeping) average >= N us preempts at once\n"
"                          (default 0: none does)\n"
"      --kick-idle         wake an idle CPU when a task is queued in a shared\n"
"                          queue (off by default: no gain measured in E2k)\n"
"      --no-kick-idle      the default; kept so older configs still run\n"
"      --wake-sync         --split: on a sync wakeup, put the woken task on the\n"
"                          waker's CPU as the kernel's default does (if that\n"
"                          CPU has nothing queued and some CPU is idle)\n"
"      --ml-trace FILE     --split: every --ml-period-ms, write the LC demand\n"
"                          sample (training data for ml/train_burst.py)\n"
"      --ml-model FILE     --split: predict LC demand with this tree (from\n"
"                          ml/train_burst.py); the prediction can only grow\n"
"                          the LC pool, and is ignored once stale\n"
"      --ml-period-ms N    sampling period (default 50; the model's must match)\n"
"      --ml-ttl-ms N       a prediction is used for N ms (default 2 periods)\n"
"      --class run|policy  batch = ran --batch-min-run-us since it slept (run),\n"
"                          or has policy SCHED_BATCH/SCHED_IDLE (policy;\n"
"                          e.g. chrt --batch 0) (default run)\n"
"      --victim any|batch  which running tasks LATENCY mode may preempt: any,\n"
"                          or only those classified batch (default any)\n"
"      --batch-min-run-us N  a task is batch once it has run N us of CPU\n"
"                          since it last slept (default 20000)\n"
"  -s, --stats FILE        also write the per-second stats to FILE as CSV\n"
"      --diag FILE         count every wait for a CPU and every run, per task\n"
"                          group, and write them per second to FILE as CSV\n"
"      --diag-comm LIST    task groups for --diag: up to 5 comma-separated\n"
"                          task-name prefixes, e.g. grpcserver,zstd; all\n"
"                          other tasks form the group \"other\"\n"
"  -t, --trace             bpf_printk every enqueue (read trace_pipe)\n"
"      --trace-class       bpf_printk every batch class change\n"
"  -v, --verbose           print libbpf debug messages\n"
"  -h, --help              display this help and exit\n";

enum {
	OPT_LAT_SLICE = 256,	/* long-only options: values beyond any char */
	OPT_BATCH_SLICE,
	OPT_SLEEP_CREDIT,
	OPT_TIMER,
	OPT_FAST_SHIFT,
	OPT_SLOW_SHIFT,
	OPT_VICTIM,
	OPT_BATCH_MIN_RUN,
	OPT_TRACE_CLASS,
	OPT_LAT_TARGET,
	OPT_LATE_PCT,
	OPT_LATE_SHIFT,
	OPT_HOLD,
	OPT_HOLD_MAX,
	OPT_SIGNAL_CPUS,
	OPT_DIAG,
	OPT_DIAG_COMM,
	OPT_SPLIT,
	OPT_SPLIT_CPUS,
	OPT_LC_MIN,
	OPT_LC_MAX,
	OPT_LC_BUFFER,
	OPT_BORROW_SLICE,
	OPT_LC_HYST,
	OPT_LC_SHIFT,
	OPT_NO_BORROW_PREEMPT,
	OPT_PREEMPT_WAIT,
	OPT_BORROWER_PICK,
	OPT_LC_QUEUE,
	OPT_PREEMPT_MIN_RUN,
	OPT_KICK_IDLE,
	OPT_NO_KICK_IDLE,
	OPT_WAKE_SYNC,
	OPT_CLAIM_BORROWER,
	OPT_ML_TRACE,
	OPT_ML_MODEL,
	OPT_ML_PERIOD,
	OPT_ML_TTL,
	OPT_CLASS,
};

static const struct option long_opts[] = {
	{ "never-preempt",   no_argument,       NULL, 'n' },
	{ "force-preempt",   no_argument,       NULL, 'p' },
	{ "adaptive",        no_argument,       NULL, 'a' },
	{ "lat-target-us",   required_argument, NULL, OPT_LAT_TARGET },
	{ "late-pct",        required_argument, NULL, OPT_LATE_PCT },
	{ "late-shift",      required_argument, NULL, OPT_LATE_SHIFT },
	{ "hold-ms",         required_argument, NULL, OPT_HOLD },
	{ "hold-max-ms",     required_argument, NULL, OPT_HOLD_MAX },
	{ "signal-cpus",     required_argument, NULL, OPT_SIGNAL_CPUS },
	{ "lat-slice-us",    required_argument, NULL, OPT_LAT_SLICE },
	{ "batch-slice-us",  required_argument, NULL, OPT_BATCH_SLICE },
	{ "sleep-credit-us", required_argument, NULL, OPT_SLEEP_CREDIT },
	{ "timer-us",        required_argument, NULL, OPT_TIMER },
	{ "fast-shift",      required_argument, NULL, OPT_FAST_SHIFT },
	{ "slow-shift",      required_argument, NULL, OPT_SLOW_SHIFT },
	{ "split",           no_argument,       NULL, OPT_SPLIT },
	{ "split-cpus",      required_argument, NULL, OPT_SPLIT_CPUS },
	{ "lc-min",          required_argument, NULL, OPT_LC_MIN },
	{ "lc-max",          required_argument, NULL, OPT_LC_MAX },
	{ "lc-buffer-pct",   required_argument, NULL, OPT_LC_BUFFER },
	{ "borrow-slice-us", required_argument, NULL, OPT_BORROW_SLICE },
	{ "lc-hyst-pct",     required_argument, NULL, OPT_LC_HYST },
	{ "lc-shift",        required_argument, NULL, OPT_LC_SHIFT },
	{ "no-borrow-preempt", no_argument,     NULL, OPT_NO_BORROW_PREEMPT },
	{ "preempt-wait-us", required_argument, NULL, OPT_PREEMPT_WAIT },
	{ "borrower-pick",   required_argument, NULL, OPT_BORROWER_PICK },
	{ "lc-queue",        required_argument, NULL, OPT_LC_QUEUE },
	{ "preempt-min-run-us", required_argument, NULL, OPT_PREEMPT_MIN_RUN },
	{ "kick-idle",       no_argument,       NULL, OPT_KICK_IDLE },
	{ "no-kick-idle",    no_argument,       NULL, OPT_NO_KICK_IDLE },
	{ "wake-sync",       no_argument,       NULL, OPT_WAKE_SYNC },
	{ "claim-borrower",  no_argument,       NULL, OPT_CLAIM_BORROWER },
	{ "ml-trace",        required_argument, NULL, OPT_ML_TRACE },
	{ "ml-model",        required_argument, NULL, OPT_ML_MODEL },
	{ "ml-period-ms",    required_argument, NULL, OPT_ML_PERIOD },
	{ "ml-ttl-ms",       required_argument, NULL, OPT_ML_TTL },
	{ "class",           required_argument, NULL, OPT_CLASS },
	{ "victim",          required_argument, NULL, OPT_VICTIM },
	{ "batch-min-run-us", required_argument, NULL, OPT_BATCH_MIN_RUN },
	{ "trace-class",     no_argument,       NULL, OPT_TRACE_CLASS },
	{ "stats",           required_argument, NULL, 's' },
	{ "diag",            required_argument, NULL, OPT_DIAG },
	{ "diag-comm",       required_argument, NULL, OPT_DIAG_COMM },
	{ "trace",           no_argument,       NULL, 't' },
	{ "verbose",         no_argument,       NULL, 'v' },
	{ "help",            no_argument,       NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	exit_req = 1;
}

/* Parse a non-negative integer option or exit. */
static unsigned long long parse_num(const char *name, const char *s)
{
	char *end;
	unsigned long long v = strtoull(s, &end, 10);

	if (!*s || *end) {
		fprintf(stderr, "invalid value for --%s: %s\n", name, s);
		exit(1);
	}
	return v;
}

/* Parse a CPU list such as "0-2" or "0,2-3" into a bitmask, or exit. */
static unsigned long long parse_cpus(const char *name, const char *s)
{
	unsigned long long mask = 0;
	const char *q = s;

	while (*q) {
		char *end;
		unsigned long a = strtoul(q, &end, 10), b = a;

		if (end == q)
			goto bad;
		if (*end == '-') {
			q = end + 1;
			b = strtoul(q, &end, 10);
			if (end == q)
				goto bad;
		}
		if (a > b || b >= KAIROS_MAX_CPUS)
			goto bad;
		for (; a <= b; a++)
			mask |= 1ULL << a;
		if (*end == ',')
			end++;
		else if (*end)
			goto bad;
		q = end;
	}
	if (mask)
		return mask;
bad:
	fprintf(stderr, "invalid CPU list for --%s: %s\n", name, s);
	exit(1);
}

/* Sum each event counter over the CPUs. */
static void read_stats(struct scx_kairos *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	assert(nr_cpus > 0);
	__u64 cnts[nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * KAIROS_NR_STATS);

	for (idx = 0; idx < KAIROS_NR_STATS; idx++) {
		int cpu;

		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &idx, cnts) < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[cpu];
	}
}

/* The kernel's count of runnable tasks (all classes), to check ours. */
static long procs_running(void)
{
	char line[256];
	long n = -1;
	FILE *f = fopen("/proc/stat", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "procs_running %ld", &n) == 1)
			break;
	fclose(f);
	return n;
}

/*
 * Split a comma-separated list of task-name prefixes into the BPF side's
 * diag_comm slots and names[] (for the CSV), or exit.
 */
static void parse_diag_comm(struct scx_kairos *skel, const char *s,
			    char names[DIAG_GROUPS][DIAG_COMM_LEN])
{
	int g = 0;

	while (*s) {
		size_t n = strcspn(s, ",");

		if (!n || n >= DIAG_COMM_LEN || g >= DIAG_NAMED) {
			fprintf(stderr, "--diag-comm takes 1-%d names of 1-%d characters\n",
				DIAG_NAMED, DIAG_COMM_LEN - 1);
			exit(1);
		}
		memcpy(names[g], s, n);
		names[g][n] = '\0';
		/* rodata strings are volatile chars: copy byte by byte */
		for (size_t i = 0; i <= n; i++)
			skel->rodata->diag_comm[g][i] = names[g][i];
		g++;
		s += n + (s[n] == ',');
	}
}

/* Sum the per-CPU diagnostics counters. Every field is a __u64. */
static void read_diag(struct scx_kairos *skel, struct diag *sum)
{
	int nr_cpus = libbpf_num_possible_cpus();
	struct diag vals[nr_cpus];
	__u32 zero = 0;
	size_t n = sizeof(*sum) / sizeof(__u64);

	memset(sum, 0, sizeof(*sum));
	if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.diag_map), &zero, vals) < 0)
		return;
	for (int cpu = 0; cpu < nr_cpus; cpu++)
		for (size_t i = 0; i < n; i++)
			((__u64 *)sum)[i] += ((__u64 *)&vals[cpu])[i];
}

static const char *ready_names[KAIROS_NR_READY] = {
	[READY_WAKEUP] = "wakeup",
	[READY_PREEMPTED] = "preempted",
	[READY_SLICE_END] = "slice_end",
	[READY_OTHER] = "other",
};

/*
 * One row per second per group and kind of wait: the number of waits that
 * ended in that second, their total in ms and their count per bucket. One
 * more row per group with kind "run": count = times it started running,
 * ms = CPU time, buckets 0. And one with kind "preempt": runs that began
 * by preempting a CPU, their total length in ms, and their count per
 * bucket of run length (same bounds as the waits).
 */
static void write_diag_header(FILE *f)
{
	static const unsigned bounds[] = { DIAG_BOUNDS_US };

	fputs("unix_s,group,kind,count,ms", f);
	for (int b = 0; b < DIAG_BUCKETS - 1; b++)
		fprintf(f, ",lt%uus", bounds[b]);
	fprintf(f, ",ge%uus\n", bounds[DIAG_BUCKETS - 2]);
}

static void write_diag(FILE *f, double t, const struct diag *cur, const struct diag *prev,
		       char names[DIAG_GROUPS][DIAG_COMM_LEN])
{
	for (int g = 0; g < DIAG_GROUPS; g++) {
		const struct diag_group *c = &cur->g[g], *p = &prev->g[g];

		if (!names[g][0])
			continue;	/* unused slot */
		for (int k = 0; k < KAIROS_NR_READY; k++) {
			__u64 cnt = 0;

			for (int b = 0; b < DIAG_BUCKETS; b++)
				cnt += c->waits[k][b] - p->waits[k][b];
			fprintf(f, "%.3f,%s,%s,%llu,%.3f", t, names[g], ready_names[k],
				(unsigned long long)cnt, (c->wait_ns[k] - p->wait_ns[k]) / 1e6);
			for (int b = 0; b < DIAG_BUCKETS; b++)
				fprintf(f, ",%llu", (unsigned long long)(c->waits[k][b] - p->waits[k][b]));
			fputc('\n', f);
		}
		fprintf(f, "%.3f,%s,run,%llu,%.3f", t, names[g],
			(unsigned long long)(c->runs - p->runs), (c->run_ns - p->run_ns) / 1e6);
		for (int b = 0; b < DIAG_BUCKETS; b++)
			fputs(",0", f);
		fputc('\n', f);
		{
			__u64 cnt = 0;

			for (int b = 0; b < DIAG_BUCKETS; b++)
				cnt += c->preempt_runs[b] - p->preempt_runs[b];
			fprintf(f, "%.3f,%s,preempt,%llu,%.3f", t, names[g], (unsigned long long)cnt,
				(c->preempt_run_ns - p->preempt_run_ns) / 1e6);
			for (int b = 0; b < DIAG_BUCKETS; b++)
				fprintf(f, ",%llu", (unsigned long long)(c->preempt_runs[b] -
									 p->preempt_runs[b]));
			fputc('\n', f);
		}
	}
	fflush(f);
}

/*
 * ML burst prediction for --split (RESEARCH_LOG 3.8). Every ml_period_ms
 * the loader takes one LC demand sample, in CPUs, defined like the BPF
 * timer's: LC tasks' CPU time over the period / its length, plus the mean
 * number of LC tasks waiting in the LC queue (lc_queued_ticks /
 * timer ticks). The model sees the last K samples, newest first, and the
 * BPF side's demand EWMA:
 *   x[0] = d(t), x[1] = d(t - 1), ..., x[K-1] = d(t - K + 1), x[K] = ewma(t)
 * and predicts the largest sample of the next H periods. ml/train_burst.py
 * builds the same features from --ml-trace files; the two must agree.
 */
#define ML_MAX_K	32
#define ML_MAX_NODES	4096

struct ml_node {
	int feature;		/* -1: leaf */
	double threshold;	/* go left if x[feature] <= threshold */
	int left, right;
	double value;		/* prediction at a leaf */
};

struct ml_model {
	int k, h, period_ms, nr_nodes;
	struct ml_node nodes[ML_MAX_NODES];
};

/*
 * Read a model written by ml/train_burst.py:
 *   kairos-tree 1
 *   k <K> h <H> period_ms <P>
 *   nodes <N>
 *   <feature> <threshold> <left> <right> <value>    (N lines, node 0 = root)
 */
static int ml_load(const char *path, struct ml_model *m)
{
	FILE *f = fopen(path, "r");
	int version;

	if (!f) {
		perror(path);
		return -1;
	}
	if (fscanf(f, "kairos-tree %d k %d h %d period_ms %d nodes %d", &version, &m->k,
		   &m->h, &m->period_ms, &m->nr_nodes) != 5 || version != 1 ||
	    m->k < 1 || m->k > ML_MAX_K || m->nr_nodes < 1 || m->nr_nodes > ML_MAX_NODES)
		goto bad;
	for (int i = 0; i < m->nr_nodes; i++) {
		struct ml_node *n = &m->nodes[i];

		if (fscanf(f, "%d %lf %d %d %lf", &n->feature, &n->threshold, &n->left,
			   &n->right, &n->value) != 5 || n->feature > m->k ||
		    (n->feature >= 0 && (n->left <= i || n->right <= i ||
					 n->left >= m->nr_nodes || n->right >= m->nr_nodes)))
			goto bad;	/* children come after their parent: no cycles */
	}
	fclose(f);
	return 0;
bad:
	fprintf(stderr, "%s: not a valid kairos-tree model\n", path);
	fclose(f);
	return -1;
}

static double ml_eval(const struct ml_model *m, const double *x)
{
	int i = 0;

	while (m->nodes[i].feature >= 0)
		i = x[m->nodes[i].feature] <= m->nodes[i].threshold ?
		    m->nodes[i].left : m->nodes[i].right;
	return m->nodes[i].value;
}

struct ml_state {
	FILE *trace;
	struct ml_model *model;	/* NULL: trace only */
	int k;			/* history kept: the model's K, or ML_MAX_K */
	double hist[ML_MAX_K];	/* demand samples, newest first */
	int filled;
	__u64 last_run, last_queued, last_ticks;
	struct timespec last, start;
	__u64 nr_pred;
};

static double ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
	return (a->tv_sec - b->tv_sec) * 1e9 + (a->tv_nsec - b->tv_nsec);
}

/* One sample; with a model, also a prediction written to the BPF side. */
static void ml_step(struct scx_kairos *skel, struct ml_state *st)
{
	__u64 run = skel->bss->lc_run_ns, queued = skel->bss->lc_queued_ticks;
	__u64 ticks = skel->bss->nr_timer_ticks;
	double dt, util, wait, d, ewma = skel->bss->lc_demand / 1024.0, pred = NAN;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	dt = ts_diff_ns(&now, &st->last);
	util = dt > 0 ? (run - st->last_run) / dt : 0;
	wait = ticks > st->last_ticks ? (double)(queued - st->last_queued) /
					(ticks - st->last_ticks) : 0;
	d = util + wait;
	st->last = now;
	st->last_run = run;
	st->last_queued = queued;
	st->last_ticks = ticks;

	memmove(&st->hist[1], &st->hist[0], (st->k - 1) * sizeof(st->hist[0]));
	st->hist[0] = d;
	if (st->filled < st->k)
		st->filled++;

	if (st->model && st->filled == st->k) {
		double x[ML_MAX_K + 1];

		memcpy(x, st->hist, st->k * sizeof(x[0]));
		x[st->k] = ewma;
		pred = ml_eval(st->model, x);
		if (pred < 0)
			pred = 0;
		/* demand first, then the stamp that makes it valid */
		skel->bss->ml_demand = pred * 1024;
		__sync_synchronize();
		skel->bss->ml_stamp = skel->bss->nr_timer_ticks ? skel->bss->nr_timer_ticks : 1;
		st->nr_pred++;
	}
	if (st->trace)
		fprintf(st->trace, "%.1f,%.4f,%.4f,%.4f,%.4f,%u,%.4f\n",
			ts_diff_ns(&now, &st->start) / 1e6, util, wait, d, ewma,
			skel->bss->nr_lc, pred);
}

static const char csv_header[] =
	"unix_s,local,global,preempt,preempt_batch,no_victim,to_batch,from_batch,"
	"nr_runnable,procs_running,"
	"fast_load,slow_load,wake_rate,wait_avg_us,wait_max_us,mode,timer_ticks,"
	"lat_wakeups,lat_late,late_frac,mode_switches,latency_ms,"
	"nr_lc,lc_cpus,lc_demand,lc_changes,borrow,ml_fresh,ml_raised,kick,"
	"idle_cpus,idle_queued,preempt_cpus,delayed,late_preempt,sync,dup_pick,dup_alt\n";

int main(int argc, char **argv)
{
	struct scx_kairos *skel;
	struct bpf_link *link;
	const char *stats_path = NULL, *diag_path = NULL;
	unsigned long long split_mask = ~0ULL;
	long lc_max = -1;	/* -1: all split CPUs */
	const char *ml_trace_path = NULL, *ml_model_path = NULL;
	long ml_period_ms = 50, ml_ttl_ms = -1;	/* -1: two periods */
	static struct ml_model ml_model;	/* big; not on the stack */
	struct ml_state ml = {};
	FILE *csv = NULL, *diag_csv = NULL;
	/* group names for the diagnostics CSV; the last is everything else */
	char diag_names[DIAG_GROUPS][DIAG_COMM_LEN] = {};
	int opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	optind = 1;
	skel = SCX_OPS_OPEN(kairos_ops, scx_kairos);

	while ((opt = getopt_long(argc, argv, "apns:tvh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			skel->rodata->forced_mode = MODE_BATCH;
			break;
		case 'p':
			skel->rodata->forced_mode = MODE_LATENCY;
			break;
		case 'a':
			skel->rodata->adaptive = true;
			break;
		case OPT_LAT_TARGET:
			skel->rodata->lat_target_ns = parse_num("lat-target-us", optarg) * 1000;
			break;
		case OPT_LATE_PCT:
			/* percent -> the fixed point late_frac uses */
			skel->rodata->late_max = parse_num("late-pct", optarg) * LATE_ONE / 100;
			break;
		case OPT_LATE_SHIFT:
			skel->rodata->late_shift = parse_num("late-shift", optarg);
			break;
		case OPT_HOLD:
			skel->rodata->hold_min_ns = parse_num("hold-ms", optarg) * 1000000;
			break;
		case OPT_HOLD_MAX:
			skel->rodata->hold_max_ns = parse_num("hold-max-ms", optarg) * 1000000;
			break;
		case OPT_SIGNAL_CPUS:
			skel->rodata->signal_cpus = parse_cpus("signal-cpus", optarg);
			break;
		case OPT_LAT_SLICE:
			skel->rodata->lat_slice_ns = parse_num("lat-slice-us", optarg) * 1000;
			break;
		case OPT_BATCH_SLICE:
			skel->rodata->batch_slice_ns = parse_num("batch-slice-us", optarg) * 1000;
			break;
		case OPT_SLEEP_CREDIT:
			skel->rodata->sleep_credit_ns = parse_num("sleep-credit-us", optarg) * 1000;
			break;
		case OPT_TIMER:
			skel->rodata->timer_interval_ns = parse_num("timer-us", optarg) * 1000;
			break;
		case OPT_FAST_SHIFT:
			skel->rodata->fast_shift = parse_num("fast-shift", optarg);
			break;
		case OPT_SLOW_SHIFT:
			skel->rodata->slow_shift = parse_num("slow-shift", optarg);
			break;
		case OPT_VICTIM:
			if (!strcmp(optarg, "any")) {
				skel->rodata->victim_batch_only = false;
			} else if (!strcmp(optarg, "batch")) {
				skel->rodata->victim_batch_only = true;
			} else {
				fprintf(stderr, "--victim must be 'any' or 'batch'\n");
				return 1;
			}
			break;
		case OPT_BATCH_MIN_RUN:
			skel->rodata->batch_min_run_ns = parse_num("batch-min-run-us", optarg) * 1000;
			break;
		case OPT_TRACE_CLASS:
			skel->rodata->trace_class = true;
			break;
		case 's':
			stats_path = optarg;
			break;
		case OPT_DIAG:
			skel->rodata->diag = true;
			diag_path = optarg;
			break;
		case OPT_DIAG_COMM:
			parse_diag_comm(skel, optarg, diag_names);
			break;
		case OPT_SPLIT:
			skel->rodata->split = true;
			break;
		case OPT_SPLIT_CPUS:
			split_mask = parse_cpus("split-cpus", optarg);
			break;
		case OPT_LC_MIN:
			skel->rodata->lc_min = parse_num("lc-min", optarg);
			break;
		case OPT_LC_MAX:
			lc_max = parse_num("lc-max", optarg);
			break;
		case OPT_LC_BUFFER:
			/* percent of a CPU -> CPUs x 1024 */
			skel->rodata->lc_buffer = parse_num("lc-buffer-pct", optarg) * 1024 / 100;
			break;
		case OPT_ML_TRACE:
			ml_trace_path = optarg;
			break;
		case OPT_ML_MODEL:
			ml_model_path = optarg;
			break;
		case OPT_ML_PERIOD:
			ml_period_ms = parse_num("ml-period-ms", optarg);
			break;
		case OPT_ML_TTL:
			ml_ttl_ms = parse_num("ml-ttl-ms", optarg);
			break;
		case OPT_NO_BORROW_PREEMPT:
			skel->rodata->borrow_preempt = false;
			break;
		case OPT_BORROWER_PICK:
			if (!strcmp(optarg, "first")) {
				skel->rodata->borrower_pick = PICK_FIRST;
			} else if (!strcmp(optarg, "prev")) {
				skel->rodata->borrower_pick = PICK_PREV;
			} else if (!strcmp(optarg, "longest")) {
				skel->rodata->borrower_pick = PICK_LONGEST;
			} else {
				fprintf(stderr, "--borrower-pick must be 'first', 'prev' or 'longest'\n");
				return 1;
			}
			break;
		case OPT_LC_QUEUE:
			if (!strcmp(optarg, "shared")) {
				skel->rodata->lc_percpu = false;
			} else if (!strcmp(optarg, "percpu")) {
				skel->rodata->lc_percpu = true;
			} else {
				fprintf(stderr, "--lc-queue must be 'shared' or 'percpu'\n");
				return 1;
			}
			break;
		case OPT_PREEMPT_WAIT:
			skel->rodata->preempt_wait_ns = parse_num("preempt-wait-us", optarg) * 1000;
			break;
		case OPT_PREEMPT_MIN_RUN:
			skel->rodata->preempt_min_run_ns = parse_num("preempt-min-run-us", optarg) * 1000;
			break;
		case OPT_KICK_IDLE:
			skel->rodata->kick_idle = true;
			break;
		case OPT_NO_KICK_IDLE:
			skel->rodata->kick_idle = false;
			break;
		case OPT_WAKE_SYNC:
			skel->rodata->wake_sync = true;
			break;
		case OPT_CLAIM_BORROWER:
			skel->rodata->claim_borrower = true;
			break;
		case OPT_LC_SHIFT:
			skel->rodata->lc_shift = parse_num("lc-shift", optarg);
			break;
		case OPT_LC_HYST:
			skel->rodata->lc_hyst = parse_num("lc-hyst-pct", optarg) * 1024 / 100;
			break;
		case OPT_BORROW_SLICE:
			skel->rodata->borrow_slice_ns = parse_num("borrow-slice-us", optarg) * 1000;
			break;
		case OPT_CLASS:
			if (!strcmp(optarg, "run")) {
				skel->rodata->class_policy = false;
			} else if (!strcmp(optarg, "policy")) {
				skel->rodata->class_policy = true;
			} else {
				fprintf(stderr, "--class must be 'run' or 'policy'\n");
				return 1;
			}
			break;
		case 't':
			skel->rodata->trace_enqueue = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}
	if (skel->rodata->fast_shift > 16 || skel->rodata->slow_shift > 16 ||
	    skel->rodata->late_shift > 10 || skel->rodata->lc_shift > 16 ||
	    !skel->rodata->timer_interval_ns) {
		/* late_shift > 10 would round a 1024 sample's step to 0 */
		fprintf(stderr, "fast/slow/lc shifts must be <= 16, --late-shift <= 10, "
			"and the timer period > 0\n");
		return 1;
	}
	if (skel->rodata->late_max > LATE_ONE || !skel->rodata->hold_min_ns ||
	    skel->rodata->hold_min_ns > skel->rodata->hold_max_ns) {
		fprintf(stderr, "--late-pct must be <= 100 and 0 < --hold-ms <= --hold-max-ms\n");
		return 1;
	}

	strcpy(diag_names[DIAG_NAMED], "other");

	if (ml_trace_path || ml_model_path) {
		if (!skel->rodata->split || ml_period_ms < 1 || ml_period_ms > 1000 ||
		    1000 % ml_period_ms) {
			fprintf(stderr, "--ml-* need --split and a --ml-period-ms dividing 1000\n");
			return 1;
		}
		ml.k = ML_MAX_K;
		if (ml_model_path) {
			if (ml_load(ml_model_path, &ml_model))
				return 1;
			if (ml_model.period_ms != ml_period_ms) {
				fprintf(stderr, "model period %d ms != --ml-period-ms %ld\n",
					ml_model.period_ms, ml_period_ms);
				return 1;
			}
			ml.model = &ml_model;
			ml.k = ml_model.k;
			if (ml_ttl_ms < 0)
				ml_ttl_ms = 2 * ml_period_ms;
			/* in BPF timer ticks */
			skel->rodata->ml_ttl_ticks = ml_ttl_ms * 1000000 / skel->rodata->timer_interval_ns;
			if (!skel->rodata->ml_ttl_ticks)
				skel->rodata->ml_ttl_ticks = 1;
		}
	}

	/*
	 * Split CPUs get ranks 0, 1, ... in CPU order (the first nr_lc are
	 * the LC CPUs); the others -1.
	 */
	{
		int nr_possible = libbpf_num_possible_cpus(), nr_split = 0;

		for (int cpu = 0; cpu < KAIROS_MAX_CPUS; cpu++)
			skel->rodata->split_rank[cpu] =
				cpu < nr_possible && (split_mask >> cpu) & 1 ? nr_split++ : -1;
		if (lc_max < 0 || lc_max > nr_split)
			lc_max = nr_split;
		skel->rodata->lc_max = lc_max;
		if (skel->rodata->split &&
		    (!nr_split || skel->rodata->lc_min < 1 || skel->rodata->lc_min > lc_max)) {
			fprintf(stderr, "--split needs split CPUs and 1 <= --lc-min <= --lc-max\n");
			return 1;
		}
		if (skel->rodata->split && (skel->rodata->adaptive ||
					    skel->rodata->forced_mode != MODE_BATCH)) {
			fprintf(stderr, "--split replaces the modes: drop --adaptive/--force-preempt\n");
			return 1;
		}
		if ((skel->rodata->preempt_wait_ns || skel->rodata->preempt_min_run_ns) &&
		    (!skel->rodata->split || !skel->rodata->borrow_preempt ||
		     !skel->rodata->preempt_wait_ns)) {
			fprintf(stderr, "--preempt-wait-us needs --split and borrower preemption; "
				"--preempt-min-run-us needs --preempt-wait-us\n");
			return 1;
		}
		if (skel->rodata->lc_percpu &&
		    (!skel->rodata->split || skel->rodata->preempt_wait_ns)) {
			fprintf(stderr, "--lc-queue percpu needs --split and no --preempt-wait-us\n");
			return 1;
		}
		if (skel->rodata->wake_sync && !skel->rodata->split) {
			fprintf(stderr, "--wake-sync is for --split (the modes use the kernel's rule)\n");
			return 1;
		}
		if (skel->rodata->claim_borrower &&
		    (!skel->rodata->split || !skel->rodata->borrow_preempt)) {
			fprintf(stderr, "--claim-borrower needs --split and borrower preemption\n");
			return 1;
		}
	}

	SCX_OPS_LOAD(skel, kairos_ops, scx_kairos, uei);
	link = SCX_OPS_ATTACH(skel, kairos_ops, scx_kairos);

	if (stats_path && !csv) {
		csv = fopen(stats_path, "w");
		if (!csv) {
			perror(stats_path);
			exit_req = 1;
		} else {
			fputs(csv_header, csv);
		}
	}
	if (ml_trace_path && !ml.trace) {
		ml.trace = fopen(ml_trace_path, "w");
		if (!ml.trace) {
			perror(ml_trace_path);
			exit_req = 1;
		} else {
			fputs("t_ms,util,queued,demand,ewma,nr_lc,pred\n", ml.trace);
		}
	}
	if (diag_path && !diag_csv) {
		diag_csv = fopen(diag_path, "w");
		if (!diag_csv) {
			perror(diag_path);
			exit_req = 1;
		} else {
			write_diag_header(diag_csv);
		}
	}
	printf("mode=%s lat_slice_us=%llu batch_slice_us=%llu timer_us=%llu "
	       "fast_shift=%u slow_shift=%u victim=%s batch_min_run_us=%llu "
	       "lat_target_us=%llu late_max=%.1f%% hold_ms=%llu-%llu signal_cpus=0x%llx "
	       "split=%d split_cpus=0x%llx lc=%u-%u lc_buffer=%.2f lc_hyst=%.2f borrow_slice_us=%llu "
	       "borrow_preempt=%d borrower_pick=%s preempt_wait_us=%llu preempt_min_run_us=%llu class=%s kick_idle=%d wake_sync=%d lc_queue=%s claim_borrower=%d\n",
	       skel->rodata->split ? "split" : skel->rodata->adaptive ? "adaptive" :
	       skel->rodata->forced_mode == MODE_LATENCY ? "latency" : "batch",
	       (unsigned long long)skel->rodata->lat_slice_ns / 1000,
	       (unsigned long long)skel->rodata->batch_slice_ns / 1000,
	       (unsigned long long)skel->rodata->timer_interval_ns / 1000,
	       skel->rodata->fast_shift, skel->rodata->slow_shift,
	       skel->rodata->victim_batch_only ? "batch" : "any",
	       (unsigned long long)skel->rodata->batch_min_run_ns / 1000,
	       (unsigned long long)skel->rodata->lat_target_ns / 1000,
	       100.0 * skel->rodata->late_max / LATE_ONE,
	       (unsigned long long)skel->rodata->hold_min_ns / 1000000,
	       (unsigned long long)skel->rodata->hold_max_ns / 1000000,
	       (unsigned long long)skel->rodata->signal_cpus,
	       skel->rodata->split, split_mask, skel->rodata->lc_min, skel->rodata->lc_max,
	       skel->rodata->lc_buffer / 1024.0, skel->rodata->lc_hyst / 1024.0,
	       (unsigned long long)skel->rodata->borrow_slice_ns / 1000,
	       skel->rodata->borrow_preempt,
	       skel->rodata->borrower_pick == PICK_PREV ? "prev" :
	       skel->rodata->borrower_pick == PICK_LONGEST ? "longest" : "first",
	       (unsigned long long)skel->rodata->preempt_wait_ns / 1000,
	       (unsigned long long)skel->rodata->preempt_min_run_ns / 1000,
	       skel->rodata->class_policy ? "policy" : "run", skel->rodata->kick_idle,
	       skel->rodata->wake_sync, skel->rodata->lc_percpu ? "percpu" : "shared",
	       skel->rodata->claim_borrower);

	__u64 prev[KAIROS_NR_STATS] = {};
	struct diag diag_prev, diag_cur;
	/* previous values of the bss counters, for per-second deltas */
	__u64 prev_lw = skel->bss->lat_wakeups, prev_ll = skel->bss->lat_late;
	__u64 prev_sw = skel->bss->nr_mode_switches, prev_lat_ns = skel->bss->mode_ns[MODE_LATENCY];
	__u64 prev_lc_ns = skel->bss->lc_cpus_ns, prev_lc_ch = skel->bss->nr_lc_changes;
	__u64 prev_mlf = skel->bss->ml_fresh_ticks, prev_mlr = skel->bss->ml_raised_ticks;
	__u64 prev_idle_ns = skel->bss->idle_cpus_ns, prev_idleq_ns = skel->bss->idle_queued_ns;
	__u64 prev_pre_on[KAIROS_MAX_CPUS];
	int nr_cpu_cols = libbpf_num_possible_cpus();

	if (nr_cpu_cols > KAIROS_MAX_CPUS)
		nr_cpu_cols = KAIROS_MAX_CPUS;
	memcpy(prev_pre_on, skel->bss->preempt_on, sizeof(prev_pre_on));

	clock_gettime(CLOCK_MONOTONIC, &ml.start);
	ml.last = ml.start;
	ml.last_run = skel->bss->lc_run_ns;
	ml.last_queued = skel->bss->lc_queued_ticks;
	ml.last_ticks = skel->bss->nr_timer_ticks;
	read_stats(skel, prev);
	read_diag(skel, &diag_prev);
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[KAIROS_NR_STATS], d[KAIROS_NR_STATS];
		struct timespec ts;
		double fast, slow, wait_avg_us, wait_max_us, late_frac, lat_ms;
		double lc_cpus, lc_demand, idle_cpus, idle_queued;
		__u64 lw, ll, sw, lat_ns, lc_ns, lc_ch, mlf, mlr, idle_ns, idleq_ns;
		/* preemptions per preempted CPU in the last second, "n0;n1;..." */
		char pre_cpus[KAIROS_MAX_CPUS * 21];
		size_t pre_len = 0;
		int i;

		if (ml.trace || ml.model) {
			/* one second in ML periods (usleep drift is fine: ml_step
			 * measures the real interval) */
			for (i = 0; i < 1000 / ml_period_ms && !exit_req; i++) {
				usleep(ml_period_ms * 1000);
				ml_step(skel, &ml);
			}
			if (ml.trace)
				fflush(ml.trace);
		} else {
			sleep(1);
		}
		read_stats(skel, stats);
		for (i = 0; i < KAIROS_NR_STATS; i++)
			d[i] = stats[i] - prev[i];	/* events in the last second */
		memcpy(prev, stats, sizeof(prev));

		/* Read the per-second max, then reset it (racy; see .bpf.c). */
		wait_max_us = skel->bss->wait_max_ns / 1000.0;
		skel->bss->wait_max_ns = 0;
		fast = skel->bss->fast_load / 1024.0;
		slow = skel->bss->slow_load / 1024.0;
		wait_avg_us = skel->bss->wait_avg_ns / 1000.0;
		late_frac = (double)skel->bss->late_frac / LATE_ONE;
		lw = skel->bss->lat_wakeups;
		ll = skel->bss->lat_late;
		sw = skel->bss->nr_mode_switches;
		lat_ns = skel->bss->mode_ns[MODE_LATENCY];
		lat_ms = (lat_ns - prev_lat_ns) / 1e6;	/* of the last second */
		lc_ns = skel->bss->lc_cpus_ns;
		lc_ch = skel->bss->nr_lc_changes;
		/* nr_lc integrated over the second, over the second's length */
		lc_cpus = (lc_ns - prev_lc_ns) / 1e9;
		lc_demand = skel->bss->lc_demand / 1024.0;
		mlf = skel->bss->ml_fresh_ticks;
		mlr = skel->bss->ml_raised_ticks;
		/* CPUs x ns over a second: the average number of CPUs */
		idle_ns = skel->bss->idle_cpus_ns;
		idleq_ns = skel->bss->idle_queued_ns;
		idle_cpus = (idle_ns - prev_idle_ns) / 1e9;
		idle_queued = (idleq_ns - prev_idleq_ns) / 1e9;
		pre_cpus[0] = '\0';
		for (i = 0; i < nr_cpu_cols; i++) {
			__u64 n = skel->bss->preempt_on[i];

			pre_len += snprintf(pre_cpus + pre_len, sizeof(pre_cpus) - pre_len, "%s%llu",
					    i ? ";" : "", (unsigned long long)(n - prev_pre_on[i]));
			prev_pre_on[i] = n;
		}

		printf("local=%llu global=%llu preempt=%llu (batch %llu) no_victim=%llu "
		       "class +%llu -%llu runnable=%lld (kernel %ld) load fast=%.2f slow=%.2f "
		       "wakeups/s=%llu wait avg=%.0fus max=%.0fus kick=%llu\n",
		       d[STAT_LOCAL], d[STAT_GLOBAL], d[STAT_PREEMPT], d[STAT_PREEMPT_BATCH],
		       d[STAT_NO_VICTIM], d[STAT_TO_BATCH], d[STAT_FROM_BATCH],
		       (long long)skel->bss->nr_runnable, procs_running(), fast, slow,
		       (unsigned long long)skel->bss->wake_rate, wait_avg_us, wait_max_us,
		       d[STAT_KICK]);
		printf("  mode=%s late %llu/%llu (ewma %.1f%%) switches=%llu latency_ms=%.0f\n",
		       skel->bss->mode == MODE_LATENCY ? "latency" : "batch",
		       ll - prev_ll, lw - prev_lw, 100 * late_frac, sw - prev_sw, lat_ms);
		if (skel->rodata->split)
			printf("  split: lc_cpus now %u, avg %.2f, demand %.2f, changes %llu, borrow %llu "
			       "delayed %llu late_preempt %llu\n",
			       skel->bss->nr_lc, lc_cpus, lc_demand, lc_ch - prev_lc_ch,
			       d[STAT_BORROW], d[STAT_DELAYED], d[STAT_LATE_PREEMPT]);
		printf("  idle cpus %.2f (%.2f with a task queued) preempted per cpu %s sync %llu "
		       "dup_pick %llu (alt %llu)\n",
		       idle_cpus, idle_queued, pre_cpus, d[STAT_SYNC], d[STAT_DUP_PICK],
		       d[STAT_DUP_ALT]);
		fflush(stdout);

		if (csv) {
			clock_gettime(CLOCK_REALTIME, &ts);
			fprintf(csv, "%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%ld,%.3f,%.3f,%llu,"
				"%.1f,%.1f,%u,%llu,%llu,%llu,%.4f,%llu,%.1f,%u,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,"
				"%.3f,%.3f,%s,%llu,%llu,%llu,%llu,%llu\n",
				ts.tv_sec + ts.tv_nsec / 1e9, d[STAT_LOCAL], d[STAT_GLOBAL],
				d[STAT_PREEMPT], d[STAT_PREEMPT_BATCH], d[STAT_NO_VICTIM],
				d[STAT_TO_BATCH], d[STAT_FROM_BATCH],
				(long long)skel->bss->nr_runnable, procs_running(), fast, slow,
				(unsigned long long)skel->bss->wake_rate, wait_avg_us,
				wait_max_us, skel->bss->mode,
				(unsigned long long)skel->bss->nr_timer_ticks,
				lw - prev_lw, ll - prev_ll, late_frac, sw - prev_sw, lat_ms,
				skel->bss->nr_lc, lc_cpus, lc_demand, lc_ch - prev_lc_ch,
				d[STAT_BORROW], mlf - prev_mlf, mlr - prev_mlr, d[STAT_KICK],
				idle_cpus, idle_queued, pre_cpus, d[STAT_DELAYED],
				d[STAT_LATE_PREEMPT], d[STAT_SYNC], d[STAT_DUP_PICK],
				d[STAT_DUP_ALT]);
			fflush(csv);
		}
		if (diag_csv) {
			clock_gettime(CLOCK_REALTIME, &ts);
			read_diag(skel, &diag_cur);
			write_diag(diag_csv, ts.tv_sec + ts.tv_nsec / 1e9, &diag_cur, &diag_prev,
				   diag_names);
			diag_prev = diag_cur;
		}
		prev_lw = lw;
		prev_ll = ll;
		prev_sw = sw;
		prev_lat_ns = lat_ns;
		prev_lc_ns = lc_ns;
		prev_lc_ch = lc_ch;
		prev_mlf = mlf;
		prev_mlr = mlr;
		prev_idle_ns = idle_ns;
		prev_idleq_ns = idleq_ns;
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_kairos__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	if (csv)
		fclose(csv);
	if (diag_csv)
		fclose(diag_csv);
	if (ml.trace)
		fclose(ml.trace);
	return 0;
}
