/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions shared by the BPF policy (scx_kairos.bpf.c) and the loader
 * (scx_kairos.c).
 */
#ifndef __SCX_KAIROS_H
#define __SCX_KAIROS_H

#define KAIROS_MAX_CPUS 64

/* Wakeup preemption modes (RESEARCH_LOG section 3.2). */
enum kairos_mode {
	MODE_BATCH = 0,		/* never preempt on wakeup; long slices */
	MODE_LATENCY = 1,	/* a woken task may preempt; short slices */
};

/* Event counters in the per-CPU "stats" map; the loader sums the CPUs. */
enum kairos_stat {
	STAT_LOCAL,		/* woken task got an idle CPU in select_cpu */
	STAT_GLOBAL,		/* task inserted into the shared queue */
	STAT_PREEMPT,		/* woken task preempted a busy CPU */
	STAT_PREEMPT_BATCH,	/* ... whose current task was classified batch */
	STAT_NO_VICTIM,		/* LATENCY mode, no idle CPU and nothing to preempt */
	KAIROS_NR_STATS,	/* not NR_STATS: vmlinux.h has one */
};

#endif /* __SCX_KAIROS_H */
