/*
 * Kairos's service manager.
 *
 *   service manager    run as the manager; init starts it this way
 *
 * A service is a program that runs for as long as the system does. Each is
 * described by a file in /etc/kairos/services, named after the service:
 *
 *   # a comment
 *   class batch         lc or batch: the class Kairos Scheduler gives it
 *   run COMMAND         the command, run by /bin/sh
 *
 * The manager starts every service at boot, each in a control group of its
 * own below the group for its class, e.g. /sys/fs/cgroup/batch/crunch.
 * Every process the service ever starts is born in that group, so the group
 * is the service: the service runs while its group holds a process, and has
 * ended once the group is empty, whatever its processes did in between
 * (exited, forked and left a child behind, were killed). The scheduler takes
 * each task's class from the group it is in.
 *
 * When a service ends, the manager starts it again by the rule init uses
 * for the scheduler: at once if it had run for a minute or more; otherwise
 * after 1, 2, 4 and 8 s, giving up after five such short runs in a row.
 *
 * Each service's output goes to /run/service/NAME/log. The manager's own
 * lines go to its standard error, which init points at the console.
 *
 * The manager waits for everything in one place: a poll() over a file
 * descriptor that delivers its signals and the "events" file of each
 * service's group, which the kernel marks changed whenever the group goes
 * from empty to not empty or back.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define DESCRIPTIONS	"/etc/kairos/services"
#define CGROUPS		"/sys/fs/cgroup"
#define RUN		"/run/service"

#define MAX_SERVICES	32

/* The restart rule; the same numbers as init's for the scheduler. */
#define STEADY		60	/* a run this many seconds long isn't a failure */
#define TRIES		5	/* failures in a row before giving up */

struct service {
	char name[32];
	char class[8];		/* "lc" or "batch" */
	char run[512];		/* the command */
	char group[96];		/* its control group's folder */
	int events;		/* its group's cgroup.events, open; -1: none */
	pid_t pid;		/* its first process, until that is collected */
	int running;		/* its group holds a process */
	long started;		/* when it was started, in ms */
	long due;		/* when to start it again, in ms; -1: not planned */
	int failures;		/* short runs in a row */
};

static struct service services[MAX_SERVICES];
static int nr_services;

/* Print one line on the console, marked as coming from the manager. */
__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
	char msg[300];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	dprintf(STDERR_FILENO, "service: %s\n", msg);
}

/* Milliseconds on the clock that never jumps. */
static long milliseconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* Cut the spaces and the newline off both ends of s. */
static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

/* A name that is safe as a folder name: letters, digits, '-' and '_'. */
static int good_name(const char *name)
{
	const char *c;

	if (!*name || strlen(name) >= sizeof(services[0].name))
		return 0;
	for (c = name; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
			return 0;
	return 1;
}

/*
 * Read one description into s. Returns 0, or says what is wrong with it
 * and returns -1.
 */
static int read_description(struct service *s, const char *name)
{
	char path[300], line[600], *key, *value;
	FILE *f;
	int ok = 1;

	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	snprintf(path, sizeof(path), "%s/%s", DESCRIPTIONS, name);
	f = fopen(path, "r");
	if (!f) {
		say("%s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		key = trim(line);
		if (!*key || *key == '#')
			continue;
		value = key + strcspn(key, " \t");
		if (*value)
			*value++ = '\0';
		value = trim(value);
		if (!strcmp(key, "class") &&
		    (!strcmp(value, "lc") || !strcmp(value, "batch"))) {
			snprintf(s->class, sizeof(s->class), "%s", value);
		} else if (!strcmp(key, "run") && *value &&
			   strlen(value) < sizeof(s->run)) {
			snprintf(s->run, sizeof(s->run), "%s", value);
		} else {
			say("%s: can't use the line \"%s %s\"", path, key, value);
			ok = 0;
		}
	}
	fclose(f);
	if (!*s->class || !*s->run) {
		say("%s: needs a class (lc or batch) and a run line", path);
		ok = 0;
	}
	/* Via a copy: snprintf() may not read from what it writes to. */
	snprintf(path, sizeof(s->group), "%s/%s/%s", CGROUPS, s->class, s->name);
	memcpy(s->group, path, sizeof(s->group));
	s->events = -1;
	s->due = -1;
	return ok ? 0 : -1;
}

/* Read every description in DESCRIPTIONS. */
static void read_descriptions(void)
{
	struct dirent *e;
	DIR *dir;

	dir = opendir(DESCRIPTIONS);
	if (!dir) {
		say("%s: %s", DESCRIPTIONS, strerror(errno));
		return;
	}
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.')
			continue;
		if (!good_name(e->d_name)) {
			say("%s/%s: a service's name is letters, digits, - and _",
			    DESCRIPTIONS, e->d_name);
			continue;
		}
		if (nr_services == MAX_SERVICES) {
			say("more than %d services; leaving out %s", MAX_SERVICES, e->d_name);
			continue;
		}
		if (read_description(&services[nr_services], e->d_name) == 0)
			nr_services++;
	}
	closedir(dir);
}

/*
 * Make the service's group and open its events file. The file says
 * "populated 1" while the group or a group below it holds a process.
 */
static int open_group(struct service *s)
{
	char path[128];

	if (mkdir(s->group, 0755) < 0 && errno != EEXIST) {
		say("can't make the control group %s: %s", s->group, strerror(errno));
		return -1;
	}
	snprintf(path, sizeof(path), "%s/cgroup.events", s->group);
	s->events = open(path, O_RDONLY | O_CLOEXEC);
	if (s->events < 0) {
		say("%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Does the service's group hold a process? 1 if so, 0 if not, -1 if the
 * file can't be read. Reading the file also tells the kernel that its
 * latest change has been seen, so poll() stops reporting it.
 */
static int populated(struct service *s)
{
	char buf[256];
	ssize_t n;

	n = pread(s->events, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return strstr(buf, "populated 1") != NULL;
}

/* Start the service: one process, in its group, running its command. */
static void start_service(struct service *s)
{
	char path[160], procs_path[128];
	sigset_t none;
	pid_t pid;
	int procs, fd;

	/* Where its output goes. */
	snprintf(path, sizeof(path), "%s/%s", RUN, s->name);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/%s/log", RUN, s->name);

	/* Opened here, written by the new process, closed when it runs sh. */
	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", s->group);
	procs = open(procs_path, O_WRONLY | O_CLOEXEC);
	if (procs < 0) {
		say("%s: can't open its group's cgroup.procs: %s", s->name, strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		say("%s: can't start a process: %s", s->name, strerror(errno));
		close(procs);
		return;
	}
	if (pid == 0) {
		/*
		 * The new process moves itself into the service's group first
		 * ("0" means the process that writes), so the service's
		 * program, and everything it ever starts, runs there.
		 */
		if (write(procs, "0", 1) < 0) {
			say("%s: can't join %s: %s", s->name, s->group, strerror(errno));
			_exit(126);
		}
		/* A session of its own; the manager's signal mask lifted. */
		setsid();
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
		fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execl("/bin/sh", "sh", "-c", s->run, (char *)NULL);
		_exit(127);
	}
	close(procs);
	s->pid = pid;
	s->running = 1;
	s->started = milliseconds();
	say("started %s (%s), process %d", s->name, s->class, pid);
}

/* The service's group is empty: decide when to start it again. */
static void service_ended(struct service *s)
{
	long now = milliseconds(), ran = (now - s->started) / 1000;
	int delay;

	s->running = 0;
	if (ran >= STEADY) {
		s->failures = 0;
		s->due = now;
		say("%s has ended, after %ld s; starting it again", s->name, ran);
		return;
	}
	if (++s->failures >= TRIES) {
		say("%s has ended within %d s of starting %d times in a row; giving up",
		    s->name, STEADY, TRIES);
		return;
	}
	delay = 1 << (s->failures - 1);
	s->due = now + delay * 1000L;
	say("%s has ended, after %ld s; starting it again in %d s (%d of %d failures before giving up)",
	    s->name, ran, delay, s->failures, TRIES);
}

/* Collect finished children: the services' first processes. */
static void collect(void)
{
	pid_t pid;
	int i;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		for (i = 0; i < nr_services; i++) {
			struct service *s = &services[i];

			if (pid != s->pid)
				continue;
			s->pid = 0;
			/*
			 * Usually the group's events file reports the end. But a
			 * process that failed before it joined the group never
			 * made it populated, so check here too.
			 */
			if (s->running && populated(s) == 0)
				service_ended(s);
		}
	}
}

static int manage(void)
{
	struct pollfd fds[1 + MAX_SERVICES];
	struct service *by_fd[1 + MAX_SERVICES];
	struct signalfd_siginfo info;
	sigset_t wanted;
	int sigfd, i, n, timeout;
	long now;

	/*
	 * The signals come through a file descriptor instead of a handler,
	 * so poll() can wait for them together with the groups' files.
	 */
	sigemptyset(&wanted);
	sigaddset(&wanted, SIGCHLD);
	sigaddset(&wanted, SIGTERM);
	sigprocmask(SIG_BLOCK, &wanted, NULL);
	sigfd = signalfd(-1, &wanted, SFD_CLOEXEC);
	if (sigfd < 0) {
		say("signalfd: %s", strerror(errno));
		return 1;
	}

	mkdir(RUN, 0755);
	read_descriptions();
	for (i = 0; i < nr_services; i++) {
		struct service *s = &services[i];

		if (open_group(s) < 0)
			continue;
		/*
		 * A service whose group already holds processes was started by
		 * a manager that has since ended. Look after it as it is.
		 */
		if (populated(s) == 1) {
			s->running = 1;
			s->started = milliseconds();
			say("%s is already running; looking after it", s->name);
		} else {
			start_service(s);
		}
	}

	for (;;) {
		/* Start what is due, and work out how long poll() may sleep. */
		now = milliseconds();
		timeout = -1;
		for (i = 0; i < nr_services; i++) {
			struct service *s = &services[i];

			if (s->due < 0)
				continue;
			if (s->due <= now) {
				s->due = -1;
				start_service(s);
			} else if (timeout < 0 || s->due - now < timeout) {
				timeout = s->due - now;
			}
		}

		fds[0].fd = sigfd;
		fds[0].events = POLLIN;
		n = 1;
		for (i = 0; i < nr_services; i++) {
			if (services[i].events < 0)
				continue;
			/* POLLPRI: the kernel's "this file changed". */
			fds[n].fd = services[i].events;
			fds[n].events = POLLPRI;
			by_fd[n++] = &services[i];
		}

		if (poll(fds, n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			say("poll: %s", strerror(errno));
			return 1;
		}

		if (fds[0].revents & POLLIN &&
		    read(sigfd, &info, sizeof(info)) == sizeof(info)) {
			if (info.ssi_signo == SIGTERM) {
				say("asked to finish; the services are left running");
				return 0;
			}
			collect();
		}

		for (i = 1; i < n; i++) {
			struct service *s = by_fd[i];

			if (!(fds[i].revents & (POLLPRI | POLLERR)))
				continue;
			if (populated(s) == 0 && s->running)
				service_ended(s);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "manager"))
		return manage();
	fprintf(stderr, "usage: service manager\n");
	return 2;
}
