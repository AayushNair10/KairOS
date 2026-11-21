/*
 * probe: a small latency-critical program, for Kairos's demo service.
 *
 * It asks to be woken at fixed times, one period apart, and does a fixed
 * amount of work each time, like a server answering a request that arrives
 * on a timer. For each wakeup it measures how late it ran: from the moment
 * it asked to be woken to the moment it was running again. With a CPU free,
 * that is the kernel's timer and wakeup cost. With every CPU busy, it is
 * also how long the scheduler made it wait.
 *
 * Every report period it prints one line: how many wakeups, and the median,
 * 99th percentile and largest delay among them.
 *
 * usage: probe [-p PERIOD_US] [-w WORK_US] [-r REPORT_S]
 *
 *   -p  time between wakeups (default 10000: 10 ms)
 *   -w  work done after each wakeup, measured on the clock (default 200).
 *       Kairos Scheduler lets an LC task whose runs average under 100 us
 *       wait up to 1 ms for a CPU to come free before it preempts batch
 *       work; one that runs longer preempts at once.
 *   -r  seconds between reports (default 10)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_SEC	1000000000LL
#define NSEC_PER_USEC	1000LL

/* Now, in nanoseconds, on the clock that never jumps. */
static long long now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * NSEC_PER_SEC + t.tv_nsec;
}

static int compare(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

/* A whole number from an option, or exit. */
static long long number(const char *s)
{
	char *end;
	long long v = strtoll(s, &end, 10);

	if (!*s || *end || v <= 0) {
		fprintf(stderr, "probe: not a positive number: %s\n", s);
		exit(2);
	}
	return v;
}

int main(int argc, char **argv)
{
	long long period = 10000 * NSEC_PER_USEC, work = 200 * NSEC_PER_USEC;
	long long report = 10 * NSEC_PER_SEC;
	long long next, woke, report_at, *late;
	long n = 0, max_n, missed = 0;
	int opt;

	while ((opt = getopt(argc, argv, "p:w:r:")) != -1) {
		switch (opt) {
		case 'p':
			period = number(optarg) * NSEC_PER_USEC;
			break;
		case 'w':
			work = number(optarg) * NSEC_PER_USEC;
			break;
		case 'r':
			report = number(optarg) * NSEC_PER_SEC;
			break;
		default:
			fprintf(stderr, "usage: probe [-p PERIOD_US] [-w WORK_US] [-r REPORT_S]\n");
			return 2;
		}
	}

	/* One report period's delays, sorted at each report. */
	max_n = report / period + 1;
	late = malloc(max_n * sizeof(*late));
	if (!late) {
		fprintf(stderr, "probe: out of memory\n");
		return 1;
	}

	printf("probe: a wakeup every %lld us, %lld us of work, a report every %lld s\n",
	       period / NSEC_PER_USEC, work / NSEC_PER_USEC, report / NSEC_PER_SEC);
	fflush(stdout);

	next = now_ns();
	report_at = next + report;
	for (;;) {
		struct timespec at;

		/*
		 * Sleep until an absolute time, not for a length of time, so the
		 * schedule doesn't drift by the time each round takes.
		 */
		next += period;
		at.tv_sec = next / NSEC_PER_SEC;
		at.tv_nsec = next % NSEC_PER_SEC;
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL) == EINTR)
			;
		woke = now_ns();
		if (n < max_n)
			late[n++] = woke - next;

		/* The work: compute until the clock says it's done. */
		while (now_ns() - woke < work)
			;

		/*
		 * If this round ran past the next wakeup time, skip the times
		 * already gone, and count them, rather than run late rounds
		 * back to back.
		 */
		while (next + period < now_ns()) {
			next += period;
			missed++;
		}

		if (woke >= report_at) {
			qsort(late, n, sizeof(*late), compare);
			printf("probe: %ld wakeups; late by: median %lld us, 99%% %lld us, most %lld us; %ld missed\n",
			       n, late[n / 2] / NSEC_PER_USEC,
			       late[n * 99 / 100] / NSEC_PER_USEC,
			       late[n - 1] / NSEC_PER_USEC, missed);
			fflush(stdout);
			n = 0;
			missed = 0;
			report_at += report;
		}
	}
}
