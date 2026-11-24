/*
 * Kairos init: the first program the kernel starts, process number 1.
 *
 * The kernel unpacks the initramfs archive into memory and starts /init.
 * Everything else that ever runs is started by this program, or by something
 * it started.
 *
 * What it does:
 *   - mounts the file systems through which the kernel shows its own
 *     information, and the control groups, with a group for each of the
 *     two classes of work that Kairos Scheduler tells apart;
 *   - starts Kairos Scheduler's loader, and waits until the kernel runs the
 *     scheduler before it starts anything else. When the loader ends, it
 *     starts it again, unless it was stopped on purpose or keeps failing.
 *     With the word kairos.noscheduler on the kernel's command line it
 *     leaves the scheduler off, and the kernel's own scheduler runs
 *     everything: the way to boot when the scheduler is what's broken;
 *   - starts the service manager, once the scheduler runs, and starts it
 *     again whenever it ends;
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
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define SHELL		"/bin/sh"
#define SELFTEST	"/selftest"

/*
 * Kairos Scheduler's loader: the program that hands the scheduling policy to
 * the kernel and holds it there for as long as it runs.
 */
#define SCHEDULER	"/bin/scx_kairos"

/*
 * Where the scheduler is pinned: a folder in the BPF file system. A pin is a
 * file that holds the scheduler, so it runs whatever happens to the loader;
 * the loader removes the pin when asked to stop, which unloads it. The
 * kernel's attach program pins it here before init starts, too.
 */
#define BPF_FS		"/sys/fs/bpf"
#define PINS		BPF_FS "/kairos"

/*
 * The service manager: it starts the services, each in a control group
 * below the group for its class, and starts them again when they end.
 */
#define MANAGER		"/bin/service"

/* Where the control groups are mounted: the usual place on Linux. */
#define CGROUPS		"/sys/fs/cgroup"

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

/* The service manager. 0 while it isn't running. */
static pid_t manager;

/* When the loader was last started, in milliseconds since boot. */
static long scheduler_started;

/* When to start it again, in milliseconds since boot. -1: no new start. */
static long scheduler_due = -1;

/* Runs in a row that ended within SCHEDULER_STEADY seconds. */
static int scheduler_failures;

/* Set once the system is shutting down: then nothing is started again. */
static int stopping;

/* Set when the control groups, lc and batch are in place. */
static int class_groups;

/* Set when the kernel keeps a class for each group, and batch's is set. */
static int kernel_classes;

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
 * Is a file system mounted on @where? A folder that is a mount point is on a
 * different device than the folder it sits in.
 */
static int mounted(const char *where)
{
	char parent[64];
	struct stat here, above;

	snprintf(parent, sizeof(parent), "%s/..", where);
	return stat(where, &here) == 0 && stat(parent, &above) == 0 &&
	       here.st_dev != above.st_dev;
}

/*
 * Mount one of the kernel's information file systems. They hold no data on
 * any disk: the kernel makes up their contents when they are read.
 *
 * One that is mounted already stays as it is. The kernel's attach program
 * mounts /sys and the BPF file system before init starts, and pins the
 * scheduler there; each mount of the BPF file system is a separate one, so
 * mounting again would hide the pins, and a new /sys would hide that mount.
 */
static int mount_info(const char *type, const char *where)
{
	if (mounted(where)) {
		say("%s is mounted already; keeping it", where);
		return 0;
	}
	if (mount(type, where, type, 0, NULL) < 0) {
		say("can't mount %s on %s: %s", type, where, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * The control groups, and Kairos's two class groups.
 *
 * A control group is a set of processes that the kernel treats as one: it
 * counts their CPU time together, can limit them together and can stop
 * them all at once. The groups are folders in a file system of their own.
 * At first every process is in the root folder, and a new process starts
 * in its parent's group; writing a process number into a group's
 * cgroup.procs file moves that process.
 *
 * Kairos makes two groups, lc and batch, one for each class of work that
 * Kairos Scheduler tells apart.
 *
 * The scheduler sees groups only through the CPU controller: to it, a task
 * is in the nearest group, going up from the task's own, that the CPU
 * controller applies to. So the CPU controller is switched on for the
 * root's children, lc and batch among them. Writing "+cpu" to the root's
 * cgroup.subtree_control does that.
 */
static int make_group(const char *path)
{
	if (mkdir(path, 0755) < 0 && errno != EEXIST) {
		say("can't make the control group %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/* Switch a controller on for the groups below @group ("+cpu", "+memory"). */
static int switch_on(const char *group, const char *controller)
{
	char path[128];
	int fd, ret = 0;

	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", group);
	fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, controller, strlen(controller)) < 0) {
		say("can't switch on %s in %s: %s", controller + 1, group, strerror(errno));
		ret = -1;
	}
	if (fd >= 0)
		close(fd);
	return ret;
}

static int switch_on_cpu_controller(void)
{
	return switch_on(CGROUPS, "+cpu");
}

/*
 * The memory controller: it charges every page of memory to the group of
 * the task that first used it, and lets the kernel protect or limit groups
 * when memory runs short. Kairos's kernel protects LC groups by their
 * class. It is switched on all the way down, below lc and batch too, so
 * that every service, and every command run with batch, has its pages
 * counted on its own; a group relabelled LC (kairos.class) then has its own
 * pages to protect. (A group can only pass a controller on to the groups
 * below it while it holds no processes itself: lc and batch never do.)
 * Without it, as in a kernel without the memory controller, memory is
 * shared as before.
 */
static void switch_on_memory_controller(void)
{
	if (switch_on(CGROUPS, "+memory") == 0) {
		switch_on(CGROUPS "/lc", "+memory");
		switch_on(CGROUPS "/batch", "+memory");
	}
}

/*
 * Kairos's kernel keeps a class for every group: its kairos controller, on
 * in every group, gives each a kairos.class file. A group's class is "lc",
 * "batch", or "inherit" (its parent's, the default); the root is LC. So
 * one write makes the batch group, and every group below it, batch. A
 * kernel without the controller has no such file.
 */
static int set_batch_class(void)
{
	int fd, ret = 0;

	fd = open(CGROUPS "/batch/kairos.class", O_WRONLY);
	if (fd < 0 && errno == ENOENT) {
		say("this kernel keeps no task classes; Kairos Scheduler will find batch work by its group");
		return -1;
	}
	if (fd < 0 || write(fd, "batch", 5) < 0) {
		say("can't set the batch group's class: %s", strerror(errno));
		ret = -1;
	}
	if (fd >= 0)
		close(fd);
	return ret;
}

static void make_class_groups(void)
{
	class_groups = mount_info("cgroup2", CGROUPS) == 0 &&
		       make_group(CGROUPS "/lc") == 0 &&
		       make_group(CGROUPS "/batch") == 0 &&
		       switch_on_cpu_controller() == 0;
	if (!class_groups)
		say("without its control groups, Kairos Scheduler takes a task's class from its scheduling policy");
	else
		switch_on_memory_controller();
	kernel_classes = class_groups && set_batch_class() == 0;
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
 * The value of a word KEY=VALUE on the kernel's command line, copied into
 * buf; NULL if there is none. (root= is the kernel's own word, but with an
 * initramfs the kernel doesn't use it: it is left for init.)
 */
static char *command_line_value(const char *key, char *buf, size_t size)
{
	char line[4096];
	size_t len = strlen(key);
	char *w;
	ssize_t n;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return NULL;
	n = read(fd, line, sizeof(line) - 1);
	close(fd);
	if (n <= 0)
		return NULL;
	line[n] = '\0';

	for (w = strtok(line, " \n"); w; w = strtok(NULL, " \n"))
		if (!strncmp(w, key, len) && w[len] == '=') {
			snprintf(buf, size, "%s", w + len + 1);
			return buf;
		}
	return NULL;
}

/*
 * The boot log on the disk: a line for every boot. It is written out at
 * once (fsync), so a boot that ends in a power cut still counts. Running
 * from memory there is no disk, and no log.
 */
#define BOOT_LOG	"/var/log/kairos/boots"

static int in_memory(void);

static void record_boot(const struct utsname *kernel)
{
	char line[160], buf[512];
	int fd, len, boots = 0;
	ssize_t n, i;

	if (in_memory())
		return;
	fd = open(BOOT_LOG, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		say("can't open %s: %s", BOOT_LOG, strerror(errno));
		return;
	}
	len = snprintf(line, sizeof(line), "kernel %s, %ld CPUs\n",
		       kernel->release, sysconf(_SC_NPROCESSORS_ONLN));
	if (write(fd, line, len) != len || fsync(fd) < 0)
		say("can't write %s: %s", BOOT_LOG, strerror(errno));
	close(fd);

	fd = open(BOOT_LOG, O_RDONLY | O_CLOEXEC);
	while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0)
		for (i = 0; i < n; i++)
			boots += buf[i] == '\n';
	if (fd >= 0)
		close(fd);
	say("boot %d of this disk (%s)", boots, BOOT_LOG);
}

/*
 * The first stage: from the initramfs to the disk.
 *
 * The kernel starts init from the initramfs, the archive it unpacked into
 * memory. With root=DEVICE on the command line (run-qemu.sh adds
 * root=/dev/vda when it attaches a disk), init makes that disk the root
 * and replaces itself with the disk's own copy of init, which carries on
 * from there: the second stage. Without root=, or if anything is wrong
 * before the switch, init stays in memory, as it always did.
 *
 * Is init still running from memory? The initramfs is a file system that
 * lives only in memory (tmpfs or ramfs); the disk is ext4.
 */
static int in_memory(void)
{
	struct statfs fs;

	return statfs("/", &fs) == 0 &&
	       (fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC);
}

#define NEWROOT		"/newroot"	/* where the disk is mounted first */
#define DISK_WAIT	10		/* seconds to wait for the disk */

/*
 * The kernel's file systems, moved onto the disk. A move takes a mount
 * with everything mounted below it: /sys carries the BPF file system and
 * the pin that holds Kairos Scheduler, so the scheduler keeps running.
 */
static const char *const moved[] = { "/proc", "/sys", "/dev" };
#define NR_MOVED	(sizeof(moved) / sizeof(moved[0]))

/* Put the first @n moved file systems back where they were. */
static void move_back(size_t n)
{
	char target[64];

	while (n-- > 0) {
		snprintf(target, sizeof(target), NEWROOT "%s", moved[n]);
		mount(target, moved[n], NULL, MS_MOVE, NULL);
	}
}

static void switch_to_disk(const char *device)
{
	char target[64];
	struct stat st;
	size_t i;
	int tries = 0, fd;

	/*
	 * 1. Wait for the disk. Its driver may still be setting it up when
	 * init starts; the kernel adds the device's entry to /dev once it
	 * has.
	 */
	while (stat(device, &st) < 0) {
		if (tries == 0)
			say("waiting for %s", device);
		if (tries++ >= DISK_WAIT * 20) {
			say("%s didn't appear in %d s; staying in memory", device, DISK_WAIT);
			return;
		}
		usleep(50 * 1000);
	}

	/*
	 * 2. Mount it, and check that it holds an init and the folders the
	 * moves need. Nothing has changed yet: if anything is wrong, init
	 * unmounts the disk and stays in memory.
	 */
	if (mkdir(NEWROOT, 0755) < 0 && errno != EEXIST) {
		say("can't make %s: %s; staying in memory", NEWROOT, strerror(errno));
		return;
	}
	if (mount(device, NEWROOT, "ext4", 0, NULL) < 0) {
		say("can't mount %s: %s; staying in memory", device, strerror(errno));
		return;
	}
	if (access(NEWROOT "/init", X_OK) < 0) {
		say("%s has no init; staying in memory", device);
		goto unmount;
	}
	for (i = 0; i < NR_MOVED; i++) {
		snprintf(target, sizeof(target), NEWROOT "%s", moved[i]);
		if (stat(target, &st) < 0 || !S_ISDIR(st.st_mode)) {
			say("%s has no %s folder; staying in memory", device, moved[i]);
			goto unmount;
		}
	}

	/* 3. Move the kernel's file systems onto the disk. */
	for (i = 0; i < NR_MOVED; i++) {
		snprintf(target, sizeof(target), NEWROOT "%s", moved[i]);
		if (mount(moved[i], target, NULL, MS_MOVE, NULL) < 0) {
			say("can't move %s onto the disk: %s; staying in memory",
			    moved[i], strerror(errno));
			move_back(i);
			goto unmount;
		}
	}

	/*
	 * 4. Make the disk the root. With the disk as the working folder,
	 * pivot_root(".", ".") puts the disk at the root and the initramfs
	 * on top of it, at the same place; unmounting "." then takes the
	 * initramfs off. MNT_DETACH: it goes from the mount table at once,
	 * and its memory is freed once nothing uses it any more. (Linux 7.0
	 * mounts the initramfs on top of an empty "nullfs", which is what
	 * lets a program pivot away from it. Older kernels forbade that, and
	 * the classic switch_root deletes the initramfs's files one by one
	 * instead. The kernel's own root mount, prepare_namespace(), uses
	 * this same pivot.)
	 */
	if (chdir(NEWROOT) < 0 || syscall(SYS_pivot_root, ".", ".") < 0) {
		say("can't make %s the root: %s; staying in memory", device, strerror(errno));
		if (chdir("/") < 0)
			say("can't go back to the initramfs's root: %s", strerror(errno));
		move_back(NR_MOVED);
		goto unmount;
	}
	if (umount2(".", MNT_DETACH) < 0)
		say("can't unmount the initramfs: %s", strerror(errno));
	if (chdir("/") < 0)
		say("can't go to the new root: %s", strerror(errno));

	/*
	 * 5. The console. The kernel opened init's input and output on the
	 * initramfs's /dev/console; kept open, they would keep the initramfs,
	 * and its memory, alive. Open them again on the moved /dev.
	 */
	fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			close(fd);
	}

	/*
	 * 6. Run the disk's init in place of this program. Running a new
	 * program keeps the process: it is still process 1 (the kernel
	 * panics if process 1 ever ends). If it can't be run, this program
	 * carries on, from memory, with the disk as the root.
	 */
	say("%s is the root now; starting its init", device);
	execl("/init", "/init", (char *)NULL);
	say("can't run the disk's init: %s; carrying on", strerror(errno));
	return;

unmount:
	umount(NEWROOT);
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
		if (pid == manager) {
			manager = 0;
			if (stopping)
				;	/* it was asked to, with everything else */
			else if (WIFSIGNALED(status))
				say("the service manager was killed by signal %d (%s)",
				    WTERMSIG(status), strsignal(WTERMSIG(status)));
			else
				say("the service manager ended with status %d",
				    WEXITSTATUS(status));
		}
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
	/*
	 * The disk, mounted read-only: ext4 finishes its journal and marks
	 * itself clean, so the next mount has nothing to replay. (Without
	 * it, the next boot's mount said "recovery complete".) The kernel's
	 * power-off doesn't do either: anything not written out by now is
	 * lost.
	 */
	if (!in_memory()) {
		if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
			say("the disk is written out and read-only");
		else
			say("can't make the disk read-only: %s", strerror(errno));
	}
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

/*
 * Start the scheduler's loader, without waiting for anything.
 *
 * If a scheduler is running already, the loader adopts it: the kernel
 * attached and pinned it before init started, or the last loader was killed
 * and the pin kept the scheduler running. The adopter loads nothing; it
 * watches the pinned scheduler and ends, with the same statuses, when the
 * scheduler does. Otherwise the loader loads it and pins it (--pin).
 */
static void launch_scheduler(void)
{
	char *const adopt[] = { SCHEDULER, "--adopt", PINS, "--quiet", NULL };
	/*
	 * --main-config: the configuration the scheduler's evaluation chose.
	 * A --class after it changes one part of it, the rule for a task's
	 * class:
	 *   kernel  the class the kernel keeps for the task's group, read at
	 *           each decision, so a change to a group's class applies at
	 *           once; or batch if the task lowered its own scheduling
	 *           policy to batch or idle.
	 *   cgroup  without the kernel's classes: batch if the task is in the
	 *           batch group or below it, or lowered its policy.
	 * Without the groups, --main-config's own rule applies: the policy
	 * alone.
	 *
	 * --split-order=layout: which CPUs form the LC pool, by the CPU layout
	 * the kernel describes in /sys: the fastest CPUs first, one memory
	 * node, cache and cluster filled before the next, and whole cores. The
	 * loader prints the order once. --quiet: no statistics on the console
	 * every second.
	 */
	char *const by_kernel[] = {
		SCHEDULER, "--main-config", "--class=kernel",
		"--split-order=layout", "--pin=" PINS, "--quiet", NULL
	};
	char *const by_group[] = {
		SCHEDULER, "--main-config", "--class=cgroup",
		"--batch-cgroup=" CGROUPS "/batch", "--split-order=layout",
		"--pin=" PINS, "--quiet", NULL
	};
	char *const by_policy[] = {
		SCHEDULER, "--main-config", "--split-order=layout", "--pin=" PINS,
		"--quiet", NULL
	};

	scheduler_started = milliseconds();
	if (scheduler_running())
		scheduler = start(adopt, 0);
	else
		scheduler = start(kernel_classes ? by_kernel :
				  class_groups ? by_group : by_policy, 0);
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
 * Kairos's kernel attaches the scheduler before it starts init, so usually
 * it runs already: init only starts a loader to adopt it, and doesn't wait.
 * A kernel without that (or with kairos.noscheduler, or after a failed
 * attach) leaves it to init.
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

	if (scheduler_running()) {
		say("Kairos Scheduler was attached before init; adopting it");
		launch_scheduler();
		return;
	}
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
	char *const svc[] = { MANAGER, "manager", NULL };
	long manager_started = -1;	/* when the manager was last started */
	struct utsname kernel;
	long started = -1;	/* when the shell was last started */
	int test;

	if (getpid() != 1) {
		fprintf(stderr, "init: this is the system's first program; it can't be run by hand\n");
		return 1;
	}

	mount_info("proc", "/proc");		/* processes and kernel state */
	mount_info("sysfs", "/sys");		/* devices and kernel settings */
	mount_info("bpf", BPF_FS);		/* pinned BPF objects */
	mount_info("devtmpfs", "/dev");		/* one entry per device */

	/* The first stage: from memory to the disk, if one is named. */
	if (in_memory()) {
		char device[64];

		if (command_line_value("root", device, sizeof(device)))
			switch_to_disk(device);
	}

	/*
	 * Files about this boot only, such as the service manager's records
	 * and the services' logs: in memory, so that none of them outlives
	 * the boot. (Mounted after the switch: anything mounted on the
	 * initramfs goes with it.)
	 */
	mount_info("tmpfs", "/run");
	make_class_groups();			/* /sys/fs/cgroup, lc, batch */

	/* Where the shell looks for programs. Everything is in /bin. */
	setenv("PATH", "/bin", 1);

	uname(&kernel);
	say("Kairos on Linux %s", kernel.release);
	record_boot(&kernel);

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

	/*
	 * The off switch. Without a start here, nothing else ever starts the
	 * scheduler: the restart rule only acts when a loader init started
	 * has ended.
	 */
	if (on_command_line("kairos.noscheduler"))
		say("kairos.noscheduler: leaving Kairos Scheduler off; the kernel's own scheduler runs everything");
	else
		start_scheduler();

	for (;;) {
		/*
		 * The service manager: started here once, before the shell,
		 * and again whenever it ends, at most once a second. The
		 * services it started keep running meanwhile; a new manager
		 * finds them by their groups and looks after them.
		 */
		if (!manager) {
			if (manager_started >= 0) {
				say("starting the service manager again");
				if (seconds() == manager_started)
					sleep(1);
			}
			manager_started = seconds();
			manager = start(svc, 0);
		}

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
