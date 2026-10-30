/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_kairos loader: sets the tunables, loads and attaches the BPF policy,
 * then prints its statistics once a second until Ctrl-C (SIGINT/SIGTERM).
 *
 * Based on scx_simple's loader: Copyright (c) 2022 Meta Platforms, Inc. and
 * affiliates; Tejun Heo <tj@kernel.org>; David Vernet <dvernet@meta.com>.
 */
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
"KairOS: load-adaptive wakeup preemption (phase 4: forced modes only).\n"
"\n"
"See the top-level comment in scx_kairos.bpf.c for more details.\n"
"\n"
"Usage: %s [options]\n"
"\n"
"  -n, --never-preempt     BATCH mode: woken tasks never preempt (default)\n"
"  -p, --force-preempt     LATENCY mode: woken tasks preempt a busy CPU\n"
"      --lat-slice-us N    slice in LATENCY mode (default 3000)\n"
"      --batch-slice-us N  slice in BATCH mode (default 20000)\n"
"      --sleep-credit-us N vtime credit kept by a sleeping task (default 20000)\n"
"      --timer-us N        load-signal timer period (default 1000)\n"
"      --fast-shift N      fast EWMA weight 1/2^N per timer tick (default 3)\n"
"      --slow-shift N      slow EWMA weight 1/2^N per timer tick (default 6)\n"
"      --victim any|batch  which running tasks LATENCY mode may preempt: any,\n"
"                          or only those classified batch (default any)\n"
"      --batch-vol-max N   a task is batch while its EWMA of voluntary stops\n"
"                          per stop (x1024) is below N (default 512)\n"
"      --class-shift N     that EWMA's weight 1/2^N per stop (default 3)\n"
"  -s, --stats FILE        also write the per-second stats to FILE as CSV\n"
"  -t, --trace             bpf_printk every enqueue (read trace_pipe)\n"
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
	OPT_BATCH_VOL_MAX,
	OPT_CLASS_SHIFT,
};

static const struct option long_opts[] = {
	{ "never-preempt",   no_argument,       NULL, 'n' },
	{ "force-preempt",   no_argument,       NULL, 'p' },
	{ "lat-slice-us",    required_argument, NULL, OPT_LAT_SLICE },
	{ "batch-slice-us",  required_argument, NULL, OPT_BATCH_SLICE },
	{ "sleep-credit-us", required_argument, NULL, OPT_SLEEP_CREDIT },
	{ "timer-us",        required_argument, NULL, OPT_TIMER },
	{ "fast-shift",      required_argument, NULL, OPT_FAST_SHIFT },
	{ "slow-shift",      required_argument, NULL, OPT_SLOW_SHIFT },
	{ "victim",          required_argument, NULL, OPT_VICTIM },
	{ "batch-vol-max",   required_argument, NULL, OPT_BATCH_VOL_MAX },
	{ "class-shift",     required_argument, NULL, OPT_CLASS_SHIFT },
	{ "stats",           required_argument, NULL, 's' },
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

static const char csv_header[] =
	"unix_s,local,global,preempt,preempt_batch,no_victim,nr_runnable,procs_running,"
	"fast_load,slow_load,wake_rate,wait_avg_us,wait_max_us,mode,timer_ticks\n";

int main(int argc, char **argv)
{
	struct scx_kairos *skel;
	struct bpf_link *link;
	const char *stats_path = NULL;
	FILE *csv = NULL;
	int opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	optind = 1;
	skel = SCX_OPS_OPEN(kairos_ops, scx_kairos);

	while ((opt = getopt_long(argc, argv, "nps:tvh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			skel->rodata->forced_mode = MODE_BATCH;
			break;
		case 'p':
			skel->rodata->forced_mode = MODE_LATENCY;
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
		case OPT_BATCH_VOL_MAX:
			skel->rodata->batch_vol_max = parse_num("batch-vol-max", optarg);
			break;
		case OPT_CLASS_SHIFT:
			skel->rodata->class_shift = parse_num("class-shift", optarg);
			break;
		case 's':
			stats_path = optarg;
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
	    skel->rodata->class_shift > 10 || skel->rodata->batch_vol_max > 1024 ||
	    !skel->rodata->timer_interval_ns) {
		/* class_shift > 10 would make the 1024 sample's step 0 */
		fprintf(stderr, "fast/slow shifts must be <= 16, the class shift <= 10, "
			"batch-vol-max <= 1024 and the timer period > 0\n");
		return 1;
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
	printf("mode=%s lat_slice_us=%llu batch_slice_us=%llu timer_us=%llu "
	       "fast_shift=%u slow_shift=%u victim=%s batch_vol_max=%u class_shift=%u\n",
	       skel->rodata->forced_mode == MODE_LATENCY ? "latency" : "batch",
	       (unsigned long long)skel->rodata->lat_slice_ns / 1000,
	       (unsigned long long)skel->rodata->batch_slice_ns / 1000,
	       (unsigned long long)skel->rodata->timer_interval_ns / 1000,
	       skel->rodata->fast_shift, skel->rodata->slow_shift,
	       skel->rodata->victim_batch_only ? "batch" : "any",
	       skel->rodata->batch_vol_max, skel->rodata->class_shift);

	__u64 prev[KAIROS_NR_STATS] = {};
	read_stats(skel, prev);
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[KAIROS_NR_STATS], d[KAIROS_NR_STATS];
		struct timespec ts;
		double fast, slow, wait_avg_us, wait_max_us;
		int i;

		sleep(1);
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

		printf("local=%llu global=%llu preempt=%llu (batch %llu) no_victim=%llu "
		       "runnable=%lld (kernel %ld) load fast=%.2f slow=%.2f wakeups/s=%llu "
		       "wait avg=%.0fus max=%.0fus\n",
		       d[STAT_LOCAL], d[STAT_GLOBAL], d[STAT_PREEMPT], d[STAT_PREEMPT_BATCH],
		       d[STAT_NO_VICTIM],
		       (long long)skel->bss->nr_runnable, procs_running(), fast, slow,
		       (unsigned long long)skel->bss->wake_rate, wait_avg_us, wait_max_us);
		fflush(stdout);

		if (csv) {
			clock_gettime(CLOCK_REALTIME, &ts);
			fprintf(csv, "%.3f,%llu,%llu,%llu,%llu,%llu,%lld,%ld,%.3f,%.3f,%llu,%.1f,%.1f,%u,%llu\n",
				ts.tv_sec + ts.tv_nsec / 1e9, d[STAT_LOCAL], d[STAT_GLOBAL],
				d[STAT_PREEMPT], d[STAT_PREEMPT_BATCH], d[STAT_NO_VICTIM],
				(long long)skel->bss->nr_runnable, procs_running(), fast, slow,
				(unsigned long long)skel->bss->wake_rate, wait_avg_us,
				wait_max_us, skel->bss->mode,
				(unsigned long long)skel->bss->nr_timer_ticks);
			fflush(csv);
		}
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_kairos__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	if (csv)
		fclose(csv);
	return 0;
}
