/*
 * Kairos init: the first program the kernel starts, process number 1.
 *
 * The kernel unpacks the initramfs archive into memory and starts /init.
 * Everything else that ever runs is started by this program, or by something
 * it started.
 *
 * What it does:
 *   - mounts the file systems through which the kernel shows its own
 *     information;
 *   - starts Kairos Scheduler's loader, and waits until the kernel runs the
 *     scheduler before it starts anything else. When the loader ends, it
 *     starts it again, unless it was stopped on purpose or keeps failing;
 *   - starts the shell, with the terminal behind the console as its own.
 *     With the word kairos.test on the kernel's command line it starts
 *     /selftest instead, for an unattended boot test;
 *   - collects every process that finishes, and starts a new shell when the
 *     shell was one of them;
 *   - shuts the system down in order when it is signalled to: asks every
 *     process to finish, waits for them, and only then tells the kernel to
 *     power off, reboot or halt.
 *
 * Process 1 must never finish. The kernel panics if it does.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define SHELL		"/bin/sh"
#define SELFTEST	"/selftest"

/*
 * Kairos Scheduler's loader: the program that hands the scheduling policy to
 * the kernel and holds it there for as long as it runs.
 */
#define SCHEDULER	"/bin/scx_kairos"

/* How many seconds a process gets to finish when it is asked to. */
#define PATIENCE	5

/*
 * How many seconds init waits for the kernel to run the scheduler before it
 * starts the shell anyway.
 */
#define SCHEDULER_WAIT	10

/*
 * The restart rule. A run of the scheduler that ends within SCHEDULER_STEADY
 * seconds counts as a failure. Init then waits before it starts it again:
 * 1 s after the first such failure, 2 s after the second, 4, 8. After
 * SCHEDULER_TRIES failures in a row it gives up. A run that lasted longer
 * starts the count afresh and is followed by a new start at once.
 */
#define SCHEDULER_STEADY	60
#define SCHEDULER_TRIES		5

/*
 * The signals init wants. It asks the kernel to hold them back ("blocks"
 * them) and takes them one at a time in main(), when it is ready for the
 * next one. So no code here ever runs in the middle of other code, the way
 * a signal handler would.
 *
 * Holding them back is also what makes the kernel keep them: a signal that
 * process 1 hasn't asked for is dropped.
 */
static sigset_t wanted;

/*
 * The process init started itself: the shell, or the self-test in its place.
 * 0 while it isn't running.
 */
static pid_t shell;

/* The scheduler's loader. 0 while it isn't running. */
static pid_t scheduler;

/* When the loader was last started, in milliseconds since boot. */
static long scheduler_started;

/* When to start it again, in milliseconds since boot. -1: no new start. */
static long scheduler_due = -1;

/* Runs in a row that ended within SCHEDULER_STEADY seconds. */
static int scheduler_failures;

/* Set once the system is shutting down: then nothing is started again. */
static int stopping;

static void scheduler_ended(int status);

/* Print one line on the console, marked as coming from init. */
__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
	char msg[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	dprintf(STDERR_FILENO, "init: %s\n", msg);
}

/*
 * Mount one of the kernel's information file systems. They hold no data on
 * any disk: the kernel makes up their contents when they are read.
 */
static void mount_info(const char *type, const char *where)
{
	if (mount(type, where, type, 0, NULL) < 0)
		say("can't mount %s on %s: %s", type, where, strerror(errno));
}

/*
 * Is this word on the kernel's command line?
 *
 * The kernel hands the words it doesn't know to init as arguments, but not
 * the ones with a dot in them: those it takes for settings of a kernel
 * module and drops. So a word like kairos.test has to be looked up here.
 */
static int on_command_line(const char *word)
{
	char line[4096];
	char *w;
	ssize_t n;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, line, sizeof(line) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	line[n] = '\0';

	for (w = strtok(line, " \n"); w; w = strtok(NULL, " \n"))
		if (!strcmp(w, word))
			return 1;
	return 0;
}

/*
 * Open the device behind the console. Returns a file descriptor, or -1.
 *
 * /dev/console is a stand-in name for "wherever the kernel's messages go".
 * It can be read and written, but the kernel never lets it be a process's
 * controlling terminal, and Ctrl-C only works through one. The kernel tells
 * us which real device is behind it. Here that is the serial port, ttyAMA0.
 */
static int open_terminal(void)
{
	char names[256], path[300];
	char *name;
	ssize_t n;
	int fd;

	fd = open("/sys/class/tty/console/active", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, names, sizeof(names) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	names[n] = '\0';
	names[strcspn(names, "\n")] = '\0';

	/* If there are several, the last one is behind /dev/console. */
	name = strrchr(names, ' ');
	name = name ? name + 1 : names;

	snprintf(path, sizeof(path), "/dev/%s", name);
	/* O_NOCTTY: just open it. Taking it as the terminal is a step apart. */
	return open(path, O_RDWR | O_NOCTTY);
}

/*
 * Start a program as a new process: argv[0] with the arguments after it.
 * Returns the new process's id, or 0 if there is no new process.
 *
 * Starting a program takes two steps. fork() makes a copy of this process.
 * The copy then replaces itself with the program, with execv(). In between,
 * the copy sets up what the program should find when it starts.
 *
 * With terminal, the program gets the console's terminal as its own, as a
 * shell needs. Without, it reads from nowhere and writes to the console, as
 * init does.
 */
static pid_t start(char *const argv[], int terminal)
{
	const char *program = argv[0];
	pid_t pid;
	int tty;

	pid = fork();
	if (pid < 0) {
		say("can't make a new process: %s", strerror(errno));
		return 0;
	}
	if (pid > 0)
		return pid;	/* still init */

	/*
	 * The copy. It has inherited init's hold on the signals, and the
	 * program would inherit it in turn, along with everything the program
	 * starts. Lift it.
	 */
	sigprocmask(SIG_UNBLOCK, &wanted, NULL);

	/*
	 * A session is a group of processes that share one terminal: a shell
	 * and what was started from it. setsid() makes this process the
	 * leader of a new session, which has no terminal yet.
	 *
	 * Every program gets a session of its own, so that what is typed at
	 * the shell can never reach the others: Ctrl-C becomes a signal only
	 * within the session whose terminal it was typed on.
	 */
	setsid();

	if (!terminal) {
		/*
		 * Input from nowhere. Output stays where init's goes, on
		 * /dev/console, which no hang-up of the terminal affects.
		 */
		tty = open("/dev/null", O_RDONLY);
		if (tty > STDIN_FILENO) {
			dup2(tty, STDIN_FILENO);
			close(tty);
		}
	} else {
		/*
		 * Give the program a terminal of its own. TIOCSCTTY takes the
		 * device as the session's terminal; only a session leader may
		 * do that, and only one session can have it. From then on
		 * Ctrl-C typed there becomes a signal to whatever the shell is
		 * running in the foreground.
		 *
		 * Last, the device becomes the program's input, output and
		 * error output. Init itself stays on /dev/console. When a
		 * session's leader finishes, the kernel hangs up the terminal
		 * for everyone who has it open, but not for those who opened
		 * it as the console.
		 */
		tty = open_terminal();
		if (tty >= 0 && ioctl(tty, TIOCSCTTY, 0) == 0) {
			dup2(tty, STDIN_FILENO);
			dup2(tty, STDOUT_FILENO);
			dup2(tty, STDERR_FILENO);
		} else {
			say("no terminal for %s; Ctrl-C won't work", program);
		}
		if (tty > STDERR_FILENO)
			close(tty);
	}

	execv(program, argv);
	say("can't run %s: %s", program, strerror(errno));
	_exit(127);
}

/*
 * Collect every process that has finished. Returns 1 while other processes
 * are still running, and 0 once there is no process left at all.
 *
 * A finished process stays in the kernel's process table, holding its exit
 * status, until its parent asks for it. Init is the parent of the process it
 * started and of every process whose own parent finished first: the kernel
 * hands those to process 1.
 */
static int collect(void)
{
	pid_t pid;
	int status;

	/*
	 * One signal can stand for several finished processes, so go on until
	 * none is left. WNOHANG: don't wait for a process to finish. status
	 * receives how the process ended: its exit status, or the signal that
	 * killed it.
	 */
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == shell)
			shell = 0;
		if (pid == scheduler) {
			scheduler = 0;
			scheduler_ended(status);
		}
	}

	/*
	 * 0 means init still has children, all of them running. Otherwise it
	 * has none. Every process without a parent becomes init's child, so
	 * "init has no children" means "nothing else is running".
	 */
	return pid == 0;
}

/*
 * Wait until no process is left, but no longer than limit seconds. Returns
 * 1 if they all finished in time.
 */
static int all_finished_within(int limit)
{
	struct timespec now, left, end;
	sigset_t finished;

	sigemptyset(&finished);
	sigaddset(&finished, SIGCHLD);

	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += limit;

	while (collect()) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		left.tv_sec = end.tv_sec - now.tv_sec;
		left.tv_nsec = end.tv_nsec - now.tv_nsec;
		if (left.tv_nsec < 0) {
			left.tv_sec--;
			left.tv_nsec += 1000000000L;
		}
		if (left.tv_sec < 0)
			return 0;
		/* Sleep until a process finishes or the time is up. */
		sigtimedwait(&finished, NULL, &left);
	}
	return 1;
}

/*
 * Shut the system down. This doesn't return.
 *
 * The kernel's reboot call does only the last step: it stops the machine
 * there and then. It doesn't tell any process, and it doesn't write out data
 * that is still only in memory. Doing those first is init's job.
 */
__attribute__((noreturn))
static void shut_down(int how, const char *what)
{
	stopping = 1;
	say("%s: asking every process to finish", what);
	/*
	 * -1 means every process except process 1. SIGTERM is the request to
	 * finish; a program may catch it and tidy up first. SIGCONT wakes the
	 * processes that were stopped (Ctrl-Z), which couldn't act on the
	 * request otherwise.
	 */
	kill(-1, SIGTERM);
	kill(-1, SIGCONT);
	/*
	 * A shell that someone is typing into ignores SIGTERM, so that a
	 * stray one can't log them out. Tell ours that its session is over
	 * with SIGHUP, the signal a shell gets when its terminal goes away.
	 * When it finishes, the kernel passes the hang-up on to whatever the
	 * shell was running in the foreground.
	 */
	if (shell)
		kill(shell, SIGHUP);

	if (all_finished_within(PATIENCE)) {
		say("every process has finished");
	} else {
		say("some are still running after %d s; stopping them", PATIENCE);
		kill(-1, SIGKILL);	/* this one can't be caught or refused */
		all_finished_within(PATIENCE);
	}

	sync();		/* write out what the file systems hold only in memory */
	reboot(how);

	say("the kernel refused: %s", strerror(errno));
	for (;;)
		pause();
}

/* Seconds since the system started. */
static long seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

/* Milliseconds since the system started. */
static long milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * Is the kernel running a BPF scheduler? The kernel's own answer is one word
 * in a file: "enabled" while it runs one. The others are "disabled" and,
 * while it switches every task over, "enabling" and "disabling".
 */
static int scheduler_running(void)
{
	char state[16];
	ssize_t n;
	int fd;

	fd = open("/sys/kernel/sched_ext/state", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, state, sizeof(state) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	state[n] = '\0';
	return !strcmp(state, "enabled\n");
}

/*
 * Wait for one of the signals init wants, but only until the time due (in
 * milliseconds since boot). Returns the signal, or -1 if the time came first.
 */
static int sleep_until(long due)
{
	long left = due - milliseconds();
	struct timespec wait;

	if (left < 0)
		left = 0;
	wait.tv_sec = left / 1000;
	wait.tv_nsec = left % 1000 * 1000000;
	return sigtimedwait(&wanted, NULL, &wait);
}

/* Start the scheduler's loader, without waiting for anything. */
static void launch_scheduler(void)
{
	/*
	 * --main-config: the configuration the scheduler's evaluation chose.
	 * --quiet: no statistics on the console every second.
	 */
	char *const argv[] = { SCHEDULER, "--main-config", "--quiet", NULL };

	scheduler_started = milliseconds();
	scheduler = start(argv, 0);
}

/*
 * What the loader's exit status says about how the scheduler ended. The
 * loader lists its statuses in its --help. 127 is init's own: the program
 * couldn't be run at all (see start()).
 *
 * *on_purpose is set when someone meant it to stop: a signal asked the
 * loader to finish (0), or the kernel unloaded the scheduler without an
 * error (3), which in this kernel means the reset key or the policy itself
 * asking to stop.
 */
static const char *how_it_ended(int status, int *on_purpose)
{
	static char text[80];
	int sig;

	*on_purpose = 0;
	if (WIFSIGNALED(status)) {
		sig = WTERMSIG(status);
		snprintf(text, sizeof(text), "was killed by signal %d (%s)",
			 sig, strsignal(sig));
		return text;
	}
	switch (WEXITSTATUS(status)) {
	case 0:
		*on_purpose = 1;
		return "was stopped on request";
	case 1:
		return "didn't start";
	case 3:
		*on_purpose = 1;
		return "was unloaded by the kernel, without an error";
	case 4:
		return "was unloaded by the kernel after an error";
	case 5:
		return "was unloaded by the kernel's watchdog: a task waited too long";
	case 127:
		return "couldn't be run";
	}
	snprintf(text, sizeof(text), "ended with status %d",
		 WEXITSTATUS(status));
	return text;
}

/*
 * The loader has ended; status says how. Say so, and decide by the restart
 * rule whether and when to start it again. The start itself happens in
 * main(), whose wait for signals ends when the time has come.
 */
static void scheduler_ended(int status)
{
	long ran = (milliseconds() - scheduler_started) / 1000;
	const char *how;
	int on_purpose, delay;

	if (stopping)
		return;

	how = how_it_ended(status, &on_purpose);
	say("Kairos Scheduler %s, after %ld s", how, ran);

	if (on_purpose) {
		say("leaving it stopped; the kernel's own scheduler runs everything");
		return;
	}
	if (ran >= SCHEDULER_STEADY) {
		scheduler_failures = 0;
		scheduler_due = milliseconds();
		say("starting it again");
		return;
	}
	if (++scheduler_failures >= SCHEDULER_TRIES) {
		say("it ended within %d s of starting %d times in a row; giving up, and the kernel's own scheduler runs everything",
		    SCHEDULER_STEADY, scheduler_failures);
		return;
	}
	delay = 1 << (scheduler_failures - 1);
	scheduler_due = milliseconds() + delay * 1000L;
	say("starting it again in %d s (%d of %d failures before init gives up)",
	    delay, scheduler_failures, SCHEDULER_TRIES);
}

/*
 * Start Kairos Scheduler at boot, and return once the kernel runs it, its
 * loader has ended, or SCHEDULER_WAIT seconds have passed.
 *
 * Waiting means that no program init starts ever runs under the kernel's
 * default scheduler; only init and the loader do, for the second or so the
 * loader takes. Nothing tells init when the scheduler is in place, so it
 * looks at the kernel's state every 10 ms, and stops looking at once when
 * the loader finishes.
 */
static void start_scheduler(void)
{
	const struct timespec step = { 0, 10 * 1000 * 1000 };
	long began = milliseconds();
	sigset_t finished;

	sigemptyset(&finished);
	sigaddset(&finished, SIGCHLD);

	say("starting Kairos Scheduler");
	launch_scheduler();

	while (scheduler) {
		if (scheduler_running()) {
			say("Kairos Scheduler is running, after %ld ms",
			    milliseconds() - began);
			return;
		}
		if (milliseconds() - began >= SCHEDULER_WAIT * 1000L) {
			say("Kairos Scheduler isn't running after %d s; going on without waiting",
			    SCHEDULER_WAIT);
			return;
		}
		/* Sleep 10 ms, or less if a process finishes. */
		if (sigtimedwait(&finished, NULL, &step) == SIGCHLD)
			collect();
	}
	/* The loader has ended, and scheduler_ended() has said what's next. */
}

int main(void)
{
	char *const sh[] = { SHELL, NULL };
	char *const selftest[] = { SELFTEST, NULL };
	struct utsname kernel;
	long started = -1;	/* when the shell was last started */
	int test;

	if (getpid() != 1) {
		fprintf(stderr, "init: this is the system's first program; it can't be run by hand\n");
		return 1;
	}

	mount_info("proc", "/proc");		/* processes and kernel state */
	mount_info("sysfs", "/sys");		/* devices and kernel settings */
	mount_info("devtmpfs", "/dev");		/* one entry per device */

	/* Where the shell looks for programs. Everything is in /bin. */
	setenv("PATH", "/bin", 1);

	uname(&kernel);
	say("Kairos on Linux %s", kernel.release);

	/*
	 * The last three are what busybox's halt, poweroff and reboot
	 * commands send to process 1.
	 */
	sigemptyset(&wanted);
	sigaddset(&wanted, SIGCHLD);	/* a child process has finished */
	sigaddset(&wanted, SIGUSR1);	/* halt */
	sigaddset(&wanted, SIGUSR2);	/* power off */
	sigaddset(&wanted, SIGTERM);	/* reboot */
	sigprocmask(SIG_BLOCK, &wanted, NULL);

	test = on_command_line("kairos.test");

	start_scheduler();

	for (;;) {
		if (!shell) {
			/* The self-test runs once, then the system powers off. */
			if (test && started >= 0)
				shut_down(RB_POWER_OFF, "power off");

			if (started >= 0)
				say("starting the shell again");
			/* At most one start a second, in case it can't run. */
			if (seconds() == started)
				sleep(1);
			started = seconds();
			shell = start(test ? selftest : sh, 1);
			if (!shell)
				continue;
		}

		/* The scheduler's new start, when its time has come. */
		if (scheduler_due >= 0 && milliseconds() >= scheduler_due) {
			scheduler_due = -1;
			launch_scheduler();
		}

		/*
		 * Sleep until one of the signals arrives, or, with a new start
		 * of the scheduler planned, until it is due. Init never sleeps
		 * through a wait in one go: it has to keep answering the other
		 * signals meanwhile, a shutdown request among them.
		 */
		switch (scheduler_due >= 0 ? sleep_until(scheduler_due) :
			sigwaitinfo(&wanted, NULL)) {
		case SIGCHLD:
			collect();
			break;
		case SIGUSR1:
			shut_down(RB_HALT_SYSTEM, "halt");
		case SIGUSR2:
			shut_down(RB_POWER_OFF, "power off");
		case SIGTERM:
			shut_down(RB_AUTOBOOT, "reboot");
		}
	}
}
