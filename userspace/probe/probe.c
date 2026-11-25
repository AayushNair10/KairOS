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
 * usage: probe [-p PERIOD_US] [-w WORK_US] [-r REPORT_S] [-n REPORTS]
 *              [-d FILE | -f FILE]
 *
 *   -p  time between wakeups (default 10000: 10 ms)
 *   -w  work done after each wakeup, measured on the clock (default 200).
 *       Kairos Scheduler lets an LC task whose runs average under 100 us
 *       wait up to 1 ms for a CPU to come free before it preempts batch
 *       work; one that runs longer preempts at once.
 *   -r  seconds between reports (default 10)
 *   -n  end after this many reports (default: never)
 *   -d  disk mode, reads: at each wakeup, instead of the work, read 4 KiB
 *       from a random place in FILE, past the page cache (O_DIRECT), so
 *       every read goes to the disk; report how long the reads took
 *   -f  disk mode, writes: at each wakeup, write 4 KiB to FILE and wait
 *       until it is on the disk (fsync); report how long that took
 */

#define _GNU_SOURCE	/* O_DIRECT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

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
	long n = 0, max_n, missed = 0, reports = -1;
	const char *disk = NULL;	/* -d or -f: the file */
	long long blocks = 0;		/* -d: 4 KiB blocks in it */
	int opt, writes = 0, fd = -1;
	void *buf = NULL;

	while ((opt = getopt(argc, argv, "p:w:r:n:d:f:")) != -1) {
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
		case 'n':
			reports = number(optarg);
			break;
		case 'd':
		case 'f':
			disk = optarg;
			writes = opt == 'f';
			break;
		default:
			fprintf(stderr, "usage: probe [-p PERIOD_US] [-w WORK_US] [-r REPORT_S] [-n REPORTS] [-d FILE | -f FILE]\n");
			return 2;
		}
	}

	/*
	 * Disk mode. Reads past the page cache need a buffer and file
	 * positions aligned to the disk's blocks: 4 KiB covers both.
	 */
	if (disk) {
		struct stat st;

		fd = writes ? open(disk, O_WRONLY | O_CREAT, 0644) : open(disk, O_RDONLY | O_DIRECT);
		if (fd < 0 || posix_memalign(&buf, 4096, 4096)) {
			fprintf(stderr, "probe: %s: %s\n", disk, strerror(errno));
			return 1;
		}
		memset(buf, 'x', 4096);
		if (!writes) {
			if (fstat(fd, &st) < 0 || st.st_size < 4096) {
				fprintf(stderr, "probe: %s: too small to read\n", disk);
				return 1;
			}
			blocks = st.st_size / 4096;
		}
		srand(getpid());
	}

	/* One report period's delays, sorted at each report. */
	max_n = report / period + 1;
	late = malloc(max_n * sizeof(*late));
	if (!late) {
		fprintf(stderr, "probe: out of memory\n");
		return 1;
	}

	if (disk)
		printf("probe: a %s every %lld us, a report every %lld s\n",
		       writes ? "write and fsync" : "read", period / NSEC_PER_USEC,
		       report / NSEC_PER_SEC);
	else
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
		if (disk) {
			/* The disk operation, timed. */
			off_t at_block = (off_t)(rand() % blocks) * 4096;
			int ok = writes ? pwrite(fd, buf, 4096, 0) == 4096 && fsync(fd) == 0 :
					  pread(fd, buf, 4096, at_block) == 4096;

			if (!ok) {
				fprintf(stderr, "probe: %s: %s\n", disk, strerror(errno));
				return 1;
			}
			if (n < max_n)
				late[n++] = now_ns() - woke;
		} else {
			if (n < max_n)
				late[n++] = woke - next;

			/* The work: compute until the clock says it's done. */
			while (now_ns() - woke < work)
				;
		}

		/*
		 * If this round ran past the next wakeup time, skip the times
		 * already gone, and count them, rather than run late rounds
		 * back to back.
		 */
		while (next + period < now_ns()) {
			next += period;
			missed++;
		}

		if (woke >= report_at && n > 0) {
			qsort(late, n, sizeof(*late), compare);
			printf(disk ? "probe: %ld %s; took: median %lld us, 99%% %lld us, most %lld us; %ld missed\n" :
				      "probe: %ld wakeups%s; late by: median %lld us, 99%% %lld us, most %lld us; %ld missed\n",
			       n, disk ? (writes ? "fsyncs" : "reads") : "", late[n / 2] / NSEC_PER_USEC,
			       late[n * 99 / 100] / NSEC_PER_USEC,
			       late[n - 1] / NSEC_PER_USEC, missed);
			fflush(stdout);
			n = 0;
			missed = 0;
			report_at += report;
			if (reports > 0 && --reports == 0)
				return 0;
		}
	}
}
