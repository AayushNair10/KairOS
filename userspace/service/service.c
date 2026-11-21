/*
 * Kairos's service manager, and the commands that talk to it.
 *
 *   service manager      run as the manager; init starts it this way
 *   service status       list the services: class, state, CPU time used
 *   service start NAME   start a service, reading its description again
 *   service stop NAME    stop a service: ask every process in its group to
 *                        finish, and kill what is left after 5 s
 *   batch COMMAND...     run one command as batch work (this program,
 *                        under another name)
 *
 * A service is a program that runs for as long as the system does. Each is
 * described by a file in /etc/kairos/services, named after the service:
 *
 *   # a comment
 *   class batch         lc or batch: the class Kairos Scheduler gives it
 *   start manual        boot (the default): started at boot; manual: only
 *                       by "service start"
 *   run COMMAND         the command, run by /bin/sh
 *
 * The manager starts each service in a control group of its own below the
 * group for its class, e.g. /sys/fs/cgroup/batch/crunch. Every process the
 * service ever starts is born in that group, so the group is the service:
 * the service runs while its group holds a process, and has ended once the
 * group is empty, whatever its processes did in between (exited, forked
 * and left a child behind, were killed). The scheduler takes each task's
 * class from the group it is in.
 *
 * When a service ends, the manager starts it again by the rule init uses
 * for the scheduler: at once if it had run for a minute or more; otherwise
 * after 1, 2, 4 and 8 s, giving up after five such short runs in a row. A
 * service stopped with "service stop" isn't started again.
 *
 * Talking to the manager. The commands write one line ("start crunch") to a
 * named pipe, /run/service/control, which the manager reads. The manager
 * keeps each service's state in /run/service/NAME/state, one line: the
 * state (running, stopping, stopped, waiting, given-up), when it began, in
 * ms on the monotonic clock, and the class. The commands read those files;
 * start and stop wait until the state is the one they asked for.
 *
 * Each service's output goes to /run/service/NAME/log. The manager's own
 * lines go to its standard error, which init points at the console.
 *
 * The manager waits for everything in one place: a poll() over a file
 * descriptor that delivers its signals, the control pipe, and the "events"
 * file of each service's group, which the kernel marks changed whenever the
 * group goes from empty to not empty or back.
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
#define CONTROL		RUN "/control"

#define MAX_SERVICES	32

/* The restart rule; the same numbers as init's for the scheduler. */
#define STEADY		60	/* a run this many seconds long isn't a failure */
#define TRIES		5	/* failures in a row before giving up */

/* Seconds a stopping service gets before the rest of it is killed. */
#define PATIENCE	5

struct service {
	char name[32];
	char class[8];		/* "lc" or "batch" */
	char run[512];		/* the command */
	char group[96];		/* its control group's folder */
	int manual;		/* started only on request */
	int events;		/* its group's cgroup.events, open; -1: none */
	pid_t pid;		/* its first process, until that is collected */
	int running;		/* started, and not yet ended */
	int seen;		/* a process was seen in its group since it started */
	int stop_asked;		/* being stopped on request: don't restart */
	long started;		/* when it was started, in ms */
	long due;		/* when to start it again, in ms; -1: not planned */
	long kill_at;		/* when to kill what is left, in ms; -1: never */
	int failures;		/* short runs in a row */
};

static struct service services[MAX_SERVICES];
static int nr_services;

static int populated(struct service *s);

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

/* Milliseconds on the clock that never jumps; the same in every process. */
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
		} else if (!strcmp(key, "start") &&
			   (!strcmp(value, "boot") || !strcmp(value, "manual"))) {
			s->manual = !strcmp(value, "manual");
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
	s->kill_at = -1;
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

static struct service *find(const char *name)
{
	int i;

	for (i = 0; i < nr_services; i++)
		if (!strcmp(services[i].name, name))
			return &services[i];
	return NULL;
}

/*
 * Make the service's group and open its events file. The file says
 * "populated 1" while the group or a group below it holds a process.
 * Also make the service's folder under RUN, for its log and state.
 */
static int open_group(struct service *s)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/%s", RUN, s->name);
	mkdir(path, 0755);

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
	/*
	 * Read it once now: the kernel reports a file it has just opened as
	 * changed until it is first read (kernfs), and poll() would wake for
	 * that at once.
	 */
	populated(s);
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

	if (s->events < 0)
		return -1;
	n = pread(s->events, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	if (strstr(buf, "populated 1")) {
		s->seen = 1;
		return 1;
	}
	return 0;
}

/*
 * Has the service ended? Its group is empty, and it had really begun:
 * a process was seen in the group, or its first process (which joins the
 * group before anything else) has finished. An empty group alone isn't
 * enough: right after a start, the new process may not have joined yet.
 *
 * Always reads the events file, so poll() stops reporting its change.
 */
static int has_ended(struct service *s)
{
	int p = populated(s);

	return s->running && p == 0 && (s->seen || !s->pid);
}

/*
 * Record the service's state for the commands. The line is written to a
 * new file that then replaces the old one in one step (rename), so a
 * reader sees either the old line or the new one, never half of one.
 */
static void set_state(struct service *s, const char *state)
{
	char path[128], tmp[136];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/state", RUN, s->name);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%s %ld %s\n", state, milliseconds(), s->class);
	fclose(f);
	rename(tmp, path);
}

/* Start the service: one process, in its group, running its command. */
static void start_service(struct service *s)
{
	char path[160], procs_path[128];
	sigset_t none;
	pid_t pid;
	int procs, fd;

	snprintf(path, sizeof(path), "%s/%s/log", RUN, s->name);

	/* Opened here, written by the new process, closed when it runs sh. */
	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", s->group);
	procs = open(procs_path, O_WRONLY | O_CLOEXEC);
	if (procs < 0) {
		say("%s: can't open its group's cgroup.procs: %s", s->name, strerror(errno));
		set_state(s, "stopped");
		return;
	}

	pid = fork();
	if (pid < 0) {
		say("%s: can't start a process: %s", s->name, strerror(errno));
		close(procs);
		set_state(s, "stopped");
		return;
	}
	s->seen = 0;
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
	set_state(s, "running");
	say("started %s (%s), process %d", s->name, s->class, pid);
}

/* Send a signal to every process in the service's group. */
static void signal_group(struct service *s, int sig)
{
	char path[128];
	FILE *f;
	int pid;

	snprintf(path, sizeof(path), "%s/cgroup.procs", s->group);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fscanf(f, "%d", &pid) == 1)
		kill(pid, sig);
	fclose(f);
}

/*
 * Kill every process in the group, all at once: the kernel does it for us
 * when "1" is written to the group's cgroup.kill, and also kills processes
 * forked while it works, which a loop over cgroup.procs could miss.
 */
static void kill_group(struct service *s)
{
	char path[128];
	int fd;

	snprintf(path, sizeof(path), "%s/cgroup.kill", s->group);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, "1", 1) < 0)
		say("%s: can't kill its group: %s", s->name, strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* The service's group is empty: decide when to start it again. */
static void service_ended(struct service *s)
{
	long now = milliseconds(), ran = (now - s->started) / 1000;
	int delay;

	s->running = 0;
	s->kill_at = -1;
	if (s->stop_asked) {
		s->stop_asked = 0;
		set_state(s, "stopped");
		say("%s has stopped", s->name);
		return;
	}
	if (ran >= STEADY) {
		s->failures = 0;
		s->due = now;
		set_state(s, "waiting");
		say("%s has ended, after %ld s; starting it again", s->name, ran);
		return;
	}
	if (++s->failures >= TRIES) {
		set_state(s, "given-up");
		say("%s has ended within %d s of starting %d times in a row; giving up",
		    s->name, STEADY, TRIES);
		return;
	}
	delay = 1 << (s->failures - 1);
	s->due = now + delay * 1000L;
	set_state(s, "waiting");
	say("%s has ended, after %ld s; starting it again in %d s (%d of %d failures before giving up)",
	    s->name, ran, delay, s->failures, TRIES);
}

/* "service stop": ask every process in the group to finish. */
static void stop_service(struct service *s)
{
	s->due = -1;
	if (!s->running) {
		set_state(s, "stopped");
		return;
	}
	if (s->stop_asked)
		return;		/* already stopping */
	s->stop_asked = 1;
	s->kill_at = milliseconds() + PATIENCE * 1000L;
	set_state(s, "stopping");
	say("stopping %s", s->name);
	/* SIGCONT wakes processes that were stopped, so they can act on it. */
	signal_group(s, SIGTERM);
	signal_group(s, SIGCONT);
}

/*
 * "service start": read the description again, which may be new or
 * changed, and start the service unless it is running.
 */
static void start_request(const char *name)
{
	struct service fresh, *s = find(name);

	if (s && s->running)
		return;
	if (read_description(&fresh, name) < 0)
		return;
	if (!s) {
		if (nr_services == MAX_SERVICES) {
			say("more than %d services; can't start %s", MAX_SERVICES, name);
			return;
		}
		s = &services[nr_services++];
		*s = fresh;
	} else {
		/* Its class may have changed, and with it its group. */
		if (strcmp(s->group, fresh.group) && s->events >= 0) {
			close(s->events);
			s->events = -1;
		}
		memcpy(s->class, fresh.class, sizeof(s->class));
		memcpy(s->run, fresh.run, sizeof(s->run));
		memcpy(s->group, fresh.group, sizeof(s->group));
		s->manual = fresh.manual;
	}
	if (s->events < 0 && open_group(s) < 0)
		return;
	s->failures = 0;
	s->due = -1;
	s->stop_asked = 0;
	start_service(s);
}

/*
 * Read the commands waiting in the control pipe: one per line. A line
 * written with one write() of fewer than PIPE_BUF (4096) bytes arrives
 * whole, never mixed with another writer's.
 */
static void read_control(int fd)
{
	char buf[2048], *line, *name, *save;
	struct service *s;
	ssize_t n;

	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return;
	buf[n] = '\0';
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		name = strchr(line, ' ');
		if (!name)
			continue;
		*name++ = '\0';
		if (!good_name(name))
			continue;
		if (!strcmp(line, "start")) {
			start_request(name);
		} else if (!strcmp(line, "stop")) {
			s = find(name);
			if (s)
				stop_service(s);
		}
	}
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
			if (has_ended(s))
				service_ended(s);
		}
	}
}

static int manage(void)
{
	struct pollfd fds[2 + MAX_SERVICES];
	struct service *by_fd[2 + MAX_SERVICES];
	struct signalfd_siginfo info;
	sigset_t wanted;
	int sigfd, control, i, n, timeout;
	long now;

	/*
	 * The signals come through a file descriptor instead of a handler,
	 * so poll() can wait for them together with everything else.
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

	/*
	 * The control pipe. Opened for reading and writing: with a writer of
	 * its own, the pipe never reports "every writer has gone" between
	 * two commands.
	 */
	mkdir(RUN, 0755);
	if (mkfifo(CONTROL, 0600) < 0 && errno != EEXIST) {
		say("%s: %s", CONTROL, strerror(errno));
		return 1;
	}
	control = open(CONTROL, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (control < 0) {
		say("%s: %s", CONTROL, strerror(errno));
		return 1;
	}

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
			set_state(s, "running");
			say("%s is already running; looking after it", s->name);
		} else if (s->manual) {
			set_state(s, "stopped");
		} else {
			start_service(s);
		}
	}

	for (;;) {
		/*
		 * Start what is due, kill what has had its time to stop, and
		 * work out how long poll() may sleep.
		 */
		now = milliseconds();
		timeout = -1;
		for (i = 0; i < nr_services; i++) {
			struct service *s = &services[i];

			if (s->due >= 0 && s->due <= now) {
				s->due = -1;
				start_service(s);
			}
			if (s->kill_at >= 0 && s->kill_at <= now) {
				s->kill_at = -1;
				say("%s still has processes after %d s; killing them",
				    s->name, PATIENCE);
				kill_group(s);
			}
			if (s->due >= 0 && (timeout < 0 || s->due - now < timeout))
				timeout = s->due - now;
			if (s->kill_at >= 0 && (timeout < 0 || s->kill_at - now < timeout))
				timeout = s->kill_at - now;
		}

		fds[0].fd = sigfd;
		fds[0].events = POLLIN;
		fds[1].fd = control;
		fds[1].events = POLLIN;
		n = 2;
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

		if (fds[1].revents & POLLIN)
			read_control(control);

		for (i = 2; i < n; i++) {
			struct service *s = by_fd[i];

			if ((fds[i].revents & (POLLPRI | POLLERR)) && has_ended(s))
				service_ended(s);
		}
	}
}

/*
 * The commands' side.
 */

/* A service's state line: state, since (ms), class. 0, or -1 if none. */
static int read_state(const char *name, char *state, long *since, char *class)
{
	char path[128];
	FILE *f;
	int n;

	snprintf(path, sizeof(path), "%s/%s/state", RUN, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fscanf(f, "%11s %ld %7s", state, since, class);
	fclose(f);
	return n == 3 ? 0 : -1;
}

/* The CPU time a group's processes have used, in seconds; -1 if unknown. */
static double cpu_seconds(const char *class, const char *name)
{
	char path[128], key[32];
	long long usec;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/%s/cpu.stat", CGROUPS, class, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fscanf(f, "%31s %lld", key, &usec) == 2) {
		if (!strcmp(key, "usage_usec")) {
			fclose(f);
			return usec / 1e6;
		}
	}
	fclose(f);
	return -1;
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* "service status": one line per service the manager has a state for. */
static int status(void)
{
	char *names[64], state[12], class[8];
	struct dirent *e;
	long since, now = milliseconds();
	int n = 0, i;
	DIR *dir;

	dir = opendir(RUN);
	if (!dir) {
		fprintf(stderr, "service: %s: %s\n", RUN, strerror(errno));
		return 1;
	}
	while ((e = readdir(dir)) && n < 64)
		if (e->d_name[0] != '.' && good_name(e->d_name))
			names[n++] = strdup(e->d_name);
	closedir(dir);
	qsort(names, n, sizeof(names[0]), compare_names);

	printf("%-12s %-6s %-9s %8s %10s\n", "NAME", "CLASS", "STATE", "FOR", "CPU USED");
	for (i = 0; i < n; i++) {
		if (read_state(names[i], state, &since, class) == 0)
			printf("%-12s %-6s %-9s %6ld s %8.1f s\n", names[i], class,
			       state, (now - since) / 1000,
			       cpu_seconds(class, names[i]));
		free(names[i]);
	}
	return 0;
}

/* Send one command line to the manager. 0, or says why not and -1. */
static int send_request(const char *what, const char *name)
{
	char line[64];
	int fd, len;

	/*
	 * O_NONBLOCK: without a reader (no manager), fail at once with
	 * ENXIO instead of waiting for one.
	 */
	fd = open(CONTROL, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "service: the service manager isn't running (%s: %s)\n",
			CONTROL, strerror(errno));
		return -1;
	}
	len = snprintf(line, sizeof(line), "%s %s\n", what, name);
	if (write(fd, line, len) != len) {
		fprintf(stderr, "service: %s: %s\n", CONTROL, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Wait up to limit seconds for the service to reach the state want, and
 * say where it got to. 0 if it got there.
 */
static int wait_for(const char *name, const char *want, int limit)
{
	const struct timespec step = { 0, 50 * 1000 * 1000 };
	char state[12] = "unknown", class[8];
	long since;
	int i;

	for (i = 0; i < limit * 20; i++) {
		if (read_state(name, state, &since, class) == 0 && !strcmp(state, want)) {
			printf("%s: %s\n", name, state);
			return 0;
		}
		nanosleep(&step, NULL);
	}
	printf("%s: %s, not %s, after %d s\n", name, state, want, limit);
	return 1;
}

/*
 * "batch COMMAND...": run the command as batch work, in a fresh group
 * below batch, then remove the group. Exits as the command did.
 */
static int run_batch(char **argv)
{
	char group[64], procs[80];
	int status, fd;
	pid_t pid;

	if (!argv[0]) {
		fprintf(stderr, "usage: batch COMMAND [ARGUMENT...]\n");
		return 2;
	}
	snprintf(group, sizeof(group), "%s/batch/cmd-%d", CGROUPS, (int)getpid());
	snprintf(procs, sizeof(procs), "%s/cgroup.procs", group);
	if (mkdir(group, 0755) < 0) {
		fprintf(stderr, "batch: %s: %s\n", group, strerror(errno));
		return 1;
	}

	pid = fork();
	if (pid == 0) {
		fd = open(procs, O_WRONLY);
		if (fd < 0 || write(fd, "0", 1) < 0) {
			fprintf(stderr, "batch: can't join %s: %s\n", group, strerror(errno));
			_exit(126);
		}
		close(fd);
		execvp(argv[0], argv);
		fprintf(stderr, "batch: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}

	/*
	 * Ctrl-C reaches the command and this program alike; let the command
	 * decide what it means, and wait for it.
	 */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	if (pid < 0 || waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "batch: %s\n", strerror(errno));
		rmdir(group);
		return 1;
	}
	/* A group can only be removed once it is empty. */
	if (rmdir(group) < 0)
		fprintf(stderr, "batch: %s still holds processes that %s left behind; it stays\n",
			group, argv[0]);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: service status\n"
		"       service start NAME\n"
		"       service stop NAME\n"
		"       service manager      (init starts it so)\n"
		"       batch COMMAND [ARGUMENT...]\n");
}

int main(int argc, char **argv)
{
	const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];

	if (!strcmp(me, "batch"))
		return run_batch(argv + 1);
	if (argc == 2 && !strcmp(argv[1], "manager"))
		return manage();
	if (argc == 2 && !strcmp(argv[1], "status"))
		return status();
	if (argc == 3 && (!strcmp(argv[1], "start") || !strcmp(argv[1], "stop"))) {
		int start = !strcmp(argv[1], "start");
		char path[128];

		if (!good_name(argv[2])) {
			fprintf(stderr, "service: a service's name is letters, digits, - and _\n");
			return 2;
		}
		/*
		 * Say at once if there is nothing to start or stop, rather than
		 * wait for a state that never comes: start needs a description,
		 * stop a service the manager has a state for.
		 */
		if (start)
			snprintf(path, sizeof(path), "%s/%s", DESCRIPTIONS, argv[2]);
		else
			snprintf(path, sizeof(path), "%s/%s/state", RUN, argv[2]);
		if (access(path, F_OK) < 0) {
			fprintf(stderr, "service: no service %s (%s: %s)\n", argv[2], path,
				strerror(errno));
			return 1;
		}
		if (send_request(argv[1], argv[2]) < 0)
			return 1;
		return wait_for(argv[2], start ? "running" : "stopped", PATIENCE + 5);
	}
	usage();
	return 2;
}
