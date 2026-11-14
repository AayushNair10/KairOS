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
 *   - starts the shell, with the terminal behind the console as its own.
 *     With the word kairos.test on the kernel's command line it starts
 *     /selftest instead, for an unattended boot test;
 *   - collects every process that finishes, and starts a new shell when the
 *     shell was one of them.
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
#include <sys/utsname.h>
#include <sys/wait.h>

#define SHELL		"/bin/sh"
#define SELFTEST	"/selftest"

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
 * Start a program as a new process. Returns the new process's id, or 0 if
 * there is no new process.
 *
 * Starting a program takes two steps. fork() makes a copy of this process.
 * The copy then replaces itself with the program, with execv(). In between,
 * the copy sets up what the program should find when it starts.
 */
static pid_t start(const char *program)
{
	char *const argv[] = { (char *)program, NULL };
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
	 * Give the program a terminal of its own.
	 *
	 * A session is a group of processes that share one terminal: a shell
	 * and what was started from it. setsid() makes this process the
	 * leader of a new session, which has no terminal yet. TIOCSCTTY then
	 * takes the device as the session's terminal; only a session leader
	 * may do that. From then on Ctrl-C typed there becomes a signal to
	 * whatever the shell is running in the foreground.
	 *
	 * Last, the device becomes the program's input, output and error
	 * output. Init itself stays on /dev/console. When a session's leader
	 * finishes, the kernel hangs up the terminal for everyone who has it
	 * open, but not for those who opened it as the console.
	 */
	setsid();
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

	execv(program, argv);
	say("can't run %s: %s", program, strerror(errno));
	_exit(127);
}

/*
 * Collect every process that has finished.
 *
 * A finished process stays in the kernel's process table, holding its exit
 * status, until its parent asks for it. Init is the parent of the process it
 * started and of every process whose own parent finished first: the kernel
 * hands those to process 1.
 */
static void collect(void)
{
	pid_t pid;

	/*
	 * One signal can stand for several finished processes, so go on until
	 * none is left. WNOHANG: don't wait for a process to finish.
	 */
	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
		if (pid == shell)
			shell = 0;
}

/* Seconds since the system started. */
static long seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

int main(void)
{
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

	sigemptyset(&wanted);
	sigaddset(&wanted, SIGCHLD);	/* a child process has finished */
	sigprocmask(SIG_BLOCK, &wanted, NULL);

	test = on_command_line("kairos.test");

	for (;;) {
		/*
		 * No shell: start one. The self-test runs only once (it cuts
		 * the power itself for now).
		 */
		if (!shell && !(test && started >= 0)) {
			if (started >= 0)
				say("starting the shell again");
			/* At most one start a second, in case it can't run. */
			if (seconds() == started)
				sleep(1);
			started = seconds();
			shell = start(test ? SELFTEST : SHELL);
			if (!shell)
				continue;
		}

		/* Sleep until one of the signals arrives. */
		sigwaitinfo(&wanted, NULL);
		collect();
	}
}
