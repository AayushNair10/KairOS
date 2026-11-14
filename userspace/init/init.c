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
 *   - starts the shell. With the word kairos.test on the kernel's command
 *     line it starts /selftest instead, for an unattended boot test;
 *   - sleeps.
 *
 * Process 1 must never finish. The kernel panics if it does.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/utsname.h>

#define SHELL		"/bin/sh"
#define SELFTEST	"/selftest"

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
 * Start a program as a new process. Returns the new process's id, or -1.
 *
 * Starting a program takes two steps. fork() makes a copy of this process.
 * The copy then replaces itself with the program, with execv().
 */
static pid_t start(const char *program)
{
	char *const argv[] = { (char *)program, NULL };
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		say("can't make a new process: %s", strerror(errno));
		return -1;
	}
	if (pid > 0)
		return pid;	/* still init */

	/* The copy. */
	execv(program, argv);
	say("can't run %s: %s", program, strerror(errno));
	_exit(127);
}

int main(void)
{
	struct utsname kernel;

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

	start(on_command_line("kairos.test") ? SELFTEST : SHELL);

	for (;;)
		pause();
}
