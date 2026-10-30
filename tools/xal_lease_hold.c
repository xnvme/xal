/**
 * xal_lease_hold -- hold a file's extents under XAL_WATCHMODE_READ_LEASE
 * ======================================================================
 *
 * Opens the FIEMAP backend in XAL_WATCHMODE_READ_LEASE, indexes, and acquires the extents of
 * <file> with xal_extents_acquire(), which takes a read lease on it. This is a test aid: it lets a
 * test check that a writer of <file> waits while the extents are held and that the extents still
 * hold the file's data meanwhile.
 *
 * Usage: xal_lease_hold [--no-recheck] [--max <n>] [--exhaust]
 *                       [--attach | --attach-rw [--write-own]] [--map-first] [--hold-open]
 *                       [--link <file2>]
 *                       [--probe | --shm | --thread | --fork | --orphan | --idle <file2> |
 *                        --sweep] <dev_uri> <mountpoint> <file>
 *
 * --no-recheck sets xal_opts.lease_no_recheck and --max xal_opts.lease_max. --attach opens <file>
 * read-only, --attach-rw read-write, and hands the descriptor to xal_extents_attach_fd() before
 * anything else, printing ATTACH err=<err> lease=<F_GETLEASE of it>; on an error, --probe exits
 * there and the other modes fail. --map-first, between that open and the attach, acquires and
 * releases <file> under a lease of xal's own, printing MAPPED err=<err>, and after the attach
 * prints LEASES=<leases on <file> in /proc/locks>. After xal_close() it prints CLOSED
 * lease=<F_GETLEASE> fd_open=<1 if the descriptor is still open>. --write-own, before LEASE_READY,
 * writes "W" at offset 0 through the descriptor, printing WROTE err=<err> lease=<F_GETLEASE>
 * byte=<the file's first byte as read through it>. With --idle, IDLE_DONE lease=<F_GETLEASE>
 * evicted=<n> follows the idle files. With an attached descriptor, --probe also prints DETACH_HELD
 * err=<err> for a detach between the acquire and the release, then, with --link <file2>, a hard
 * link of <file>, breaks the lease with its own O_NONBLOCK open for writing, prints BROKEN
 * revoked=<n> once xal has revoked it and ATTACH_LINK err=<err> for attaching the descriptor to
 * <file2>; then ATTACH_AGAIN err=<err> for attaching it again, ATTACH_DUP err=<err> for attaching a
 * dup() of it, DETACH err=<err> lease=<F_GETLEASE> fd_open=<n>, ATTACH_AFTER_DETACH err=<err>
 * lease=<F_GETLEASE> for attaching it after that, and DETACH_VALID err=<err> lease=<F_GETLEASE>
 * fd_open=<n> for detaching it with the lease held.
 * --hold-open opens <file> read-only before the attach and closes it after, printing ATTACH_RETRY
 * err=<err> lease=<F_GETLEASE> for attaching again if the attach failed. --exhaust opens
 * /dev/null until no descriptor is left, after the extents are held, and acquires no more. Without
 * a mode option, prints once the extents are held:
 *   LEASE_READY pid=<pid> byte=<device byte offset of the first extent> extents=<n>
 * then waits for SIGUSR1, releases the extents and prints RELEASED err=<what xal_extents_release()
 * returned>. It then acquires them again every 50 ms until that succeeds, which it can once the
 * writer has closed the file, and prints
 *   REACQUIRED byte=<device byte offset of the first extent> extents=<n>
 * and waits for SIGTERM or SIGINT before closing.
 *
 * --thread takes the first acquire on a thread that exits before LEASE_READY is printed, as a
 * caller's worker thread may. --idle also acquires and releases <file2>, or every file under it
 * if it is a directory, before LEASE_READY, so they are under a lease with no reader while <file>
 * is held.
 *
 * --probe acquires once, prints ACQUIRE err=<err> and, on success, one line per extent:
 *   EXTENT <i> byte=<device byte offset> bytes=<length> flag=<flag>
 * then releases twice, printing RELEASE err=<err> and RELEASE_AGAIN err=<err>, and exits. --shm
 * opens with a shm_name, prints OPEN err=<err> and exits. --fork acquires and releases, forks a
 * child that acquires, prints CHILD_ACQUIRE err=<err> and CHILD_STATS err=<what xal_lease_stats()
 * returned>, opens /dev/null 64 times, closes the handle and prints CHILD_FDS=<how many of those
 * are still open>/<how many it opened>, then prints PARENT_LEASE=<n>, the number of leases on
 * <file> listed in /proc/locks, and exits. --orphan acquires and releases, forks a child that
 * sleeps for 30 s, or until a signal, with the handle open, prints ORPHAN child=<pid> once fork()
 * has returned in the child, and exits without xal_close().
 *
 * --sweep takes <file> as a directory, acquires and releases every file under it in walk order,
 * then prints
 *   SWEEP files=<n> failed=<n> err=<first error> leases=<held by this process> evicted=<n>
 * and exits; leases is from /proc/locks and evicted from xal_lease_stats().
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libxal.h>

static volatile sig_atomic_t g_release = 0;
static bool g_exhaust;
static volatile sig_atomic_t g_stop = 0;
static int g_attach = -1; ///< open() flags of --attach or --attach-rw
static int g_fd = -1;	  ///< The attached descriptor
static bool g_write_own;
static bool g_map_first;
static bool g_hold_open;

static void
on_signal(int signo)
{
	if (signo == SIGUSR1) {
		g_release = 1;
	} else {
		g_stop = 1;
	}
}

static uint64_t
first_byte(struct xal *xal, struct xal_extents *extents)
{
	if (!extents->count) {
		return 0;
	}

	return xal_extent_at(xal, extents->extent_idx)->start_block * xal_get_sb_blocksize(xal);
}

static int
probe(struct xal *xal, struct xal_inode *inode, struct xal_inode *link)
{
	struct xal_extents *extents;
	uint32_t blocksize = xal_get_sb_blocksize(xal);
	int err, dup_fd;

	err = xal_extents_acquire(xal, inode, &extents);
	printf("ACQUIRE err=%d\n", err);
	if (err) {
		return 0;
	}

	for (uint32_t i = 0; i < extents->count; ++i) {
		struct xal_extent *extent = xal_extent_at(xal, extents->extent_idx + i);

		printf("EXTENT %u byte=%" PRIu64 " bytes=%" PRIu64 " flag=0x%02x\n", i,
		       extent->start_block * blocksize, extent->nblocks * blocksize, extent->flag);
	}
	if (g_fd >= 0) {
		printf("DETACH_HELD err=%d\n", xal_extents_detach_fd(xal, inode));
	}
	printf("RELEASE err=%d\n", xal_extents_release(xal, inode));
	printf("RELEASE_AGAIN err=%d\n", xal_extents_release(xal, inode));
	if (g_fd < 0) {
		return 0;
	}

	if (link) {
		struct timespec tick = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
		struct xal_lease_stats before = {0}, after = {0};
		char path[XAL_INODE_PATH_MAXLEN + 1];

		/* Broken, the lease leaves fd_map; the descriptor stays attached */
		xal_lease_stats(xal, &before);
		if (xal_inode_path(xal, inode, path, sizeof(path)) >= 0) {
			int fd = open(path, O_WRONLY | O_NONBLOCK);

			if (fd >= 0) {
				close(fd);
			}
		}
		for (int i = 0; i < 200 && after.revoked == before.revoked; ++i) {
			nanosleep(&tick, NULL);
			xal_lease_stats(xal, &after);
		}
		printf("BROKEN revoked=%" PRIu64 "\n", after.revoked - before.revoked);
		printf("ATTACH_LINK err=%d\n", xal_extents_attach_fd(xal, link, g_fd, NULL));
	}
	printf("ATTACH_AGAIN err=%d\n", xal_extents_attach_fd(xal, inode, g_fd, NULL));
	dup_fd = dup(g_fd);
	printf("ATTACH_DUP err=%d\n", xal_extents_attach_fd(xal, inode, dup_fd, NULL));
	close(dup_fd);
	err = xal_extents_detach_fd(xal, inode);
	printf("DETACH err=%d lease=%d fd_open=%d\n", err, fcntl(g_fd, F_GETLEASE),
	       fcntl(g_fd, F_GETFD) >= 0);
	err = xal_extents_attach_fd(xal, inode, g_fd, NULL);
	printf("ATTACH_AFTER_DETACH err=%d lease=%d\n", err, fcntl(g_fd, F_GETLEASE));
	if (!err) {
		err = xal_extents_detach_fd(xal, inode);
		printf("DETACH_VALID err=%d lease=%d fd_open=%d\n", err, fcntl(g_fd, F_GETLEASE),
		       fcntl(g_fd, F_GETFD) >= 0);
	}

	return 0;
}

/* Leases on the inode, as /proc/locks lists them: "1: LEASE ACTIVE READ <pid> <maj:min:ino> ..." */
static int
count_leases(uint64_t ino)
{
	char line[256], suffix[32];
	FILE *f = fopen("/proc/locks", "r");
	int n = 0;

	if (!f) {
		return -1;
	}
	snprintf(suffix, sizeof(suffix), ":%" PRIu64 " ", ino);
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "LEASE") && strstr(line, suffix)) {
			n++;
		}
	}
	fclose(f);

	return n;
}

static int
fork_probe(struct xal *xal, struct xal_inode *inode)
{
	struct xal_extents *extents;
	int err, status;
	pid_t pid;

	err = xal_extents_acquire(xal, inode, &extents);
	if (err) {
		fprintf(stderr, "FAILED: xal_extents_acquire(); err(%d)\n", err);
		return 1;
	}
	xal_extents_release(xal, inode);
	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		return 1;
	}
	if (!pid) {
		struct xal_lease_stats stats;
		int fds[64], opened = 0, open_fds = 0;

		printf("CHILD_ACQUIRE err=%d\n", xal_extents_acquire(xal, inode, &extents));
		printf("CHILD_STATS err=%d\n", xal_lease_stats(xal, &stats));
		fflush(stdout);
		/* These reuse the numbers of the descriptors the fork handler closed */
		for (int i = 0; i < 64; ++i) {
			fds[i] = open("/dev/null", O_RDONLY);
			opened += fds[i] >= 0;
		}
		xal_close(xal);
		for (int i = 0; i < 64; ++i) {
			open_fds += fds[i] >= 0 && fcntl(fds[i], F_GETFD) >= 0;
		}
		printf("CHILD_FDS=%d/%d\n", open_fds, opened);
		fflush(stdout);
		_exit(0);
	}
	waitpid(pid, &status, 0);

	printf("PARENT_LEASE=%d\n", count_leases(inode->ino));

	return 0;
}

static int
orphan(struct xal *xal, struct xal_inode *inode)
{
	struct xal_extents *extents;
	int err, ready[2];
	pid_t pid;
	char c;

	err = xal_extents_acquire(xal, inode, &extents);
	if (err) {
		fprintf(stderr, "FAILED: xal_extents_acquire(); err(%d)\n", err);
		return 1;
	}
	xal_extents_release(xal, inode);
	fflush(stdout);

	if (pipe(ready)) {
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		return 1;
	}
	if (!pid) {
		sigset_t set;

		/* fork() has returned, so the fork handlers have run */
		sigemptyset(&set);
		sigaddset(&set, SIGUSR1);
		sigaddset(&set, SIGTERM);
		sigaddset(&set, SIGINT);
		sigprocmask(SIG_UNBLOCK, &set, NULL);
		if (write(ready[1], "", 1) == 1) {
			sleep(30);
		}
		_exit(0);
	}
	close(ready[1]);
	if (read(ready[0], &c, 1) != 1) {
		return 1;
	}
	printf("ORPHAN child=%d\n", (int)pid);
	fflush(stdout);
	_exit(0);
}

/* Leases held by this process, as /proc/locks lists them */
static int
count_own_leases(void)
{
	char line[256], pid[32];
	FILE *f = fopen("/proc/locks", "r");
	int n = 0;

	if (!f) {
		return -1;
	}
	snprintf(pid, sizeof(pid), " %d ", (int)getpid());
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "LEASE") && strstr(line, pid)) {
			n++;
		}
	}
	fclose(f);

	return n;
}

struct sweep_args {
	uint32_t files;
	uint32_t failed;
	int err;
};

static int
sweep_one(struct xal *xal, struct xal_inode *inode, void *cb_args, int level)
{
	struct sweep_args *a = cb_args;
	struct xal_extents *extents;
	int err;

	(void)level;
	if (!xal_inode_is_file(inode)) {
		return 0;
	}

	a->files++;
	err = xal_extents_acquire(xal, inode, &extents);
	if (!err) {
		err = xal_extents_release(xal, inode);
	}
	if (err && !a->failed++) {
		a->err = err;
	}

	return 0;
}

static int
sweep(struct xal *xal, struct xal_inode *dir)
{
	struct sweep_args a = {0};
	struct xal_lease_stats stats = {0};

	xal_walk(xal, dir, sweep_one, &a);
	xal_lease_stats(xal, &stats);
	printf("SWEEP files=%u failed=%u err=%d leases=%d evicted=%" PRIu64 "\n", a.files, a.failed,
	       a.err, count_own_leases(), stats.evicted);

	return 0;
}

struct acquire_args {
	struct xal *xal;
	struct xal_inode *inode;
	struct xal_extents *extents;
	int err;
};

static void *
acquire_thread(void *arg)
{
	struct acquire_args *a = arg;

	a->err = xal_extents_acquire(a->xal, a->inode, &a->extents);

	return NULL;
}

static int
hold(struct xal *xal, struct xal_inode *inode, struct xal_inode *idle, bool on_thread,
     const sigset_t *orig)
{
	struct timespec retry = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
	struct acquire_args a = {.xal = xal, .inode = inode};
	struct xal_extents *extents;
	int err;

	if (idle && !xal_inode_is_file(idle)) {
		struct sweep_args s = {0};

		xal_walk(xal, idle, sweep_one, &s);
		if (s.failed) {
			fprintf(stderr, "FAILED: sweep of idle; err(%d)\n", s.err);
			return 1;
		}
	} else if (idle) {
		err = xal_extents_acquire(xal, idle, &extents);
		if (err) {
			fprintf(stderr, "FAILED: xal_extents_acquire(idle); err(%d)\n", err);
			return 1;
		}
		xal_extents_release(xal, idle);
	}
	if (idle && g_fd >= 0) {
		struct xal_lease_stats stats = {0};

		xal_lease_stats(xal, &stats);
		printf("IDLE_DONE lease=%d evicted=%" PRIu64 "\n", fcntl(g_fd, F_GETLEASE),
		       stats.evicted);
	}

	if (on_thread) {
		pthread_t t;

		if (pthread_create(&t, NULL, acquire_thread, &a) || pthread_join(t, NULL)) {
			fprintf(stderr, "FAILED: acquire thread\n");
			return 1;
		}
	} else {
		acquire_thread(&a);
	}
	err = a.err;
	extents = a.extents;
	if (err) {
		fprintf(stderr, "FAILED: xal_extents_acquire(); err(%d)\n", err);
		return 1;
	}
	if (g_write_own) {
		char byte = '?';

		err = pwrite(g_fd, "W", 1, 0) == 1 ? 0 : -errno;
		if (pread(g_fd, &byte, 1, 0) != 1) {
			byte = '?';
		}
		printf("WROTE err=%d lease=%d byte=%c\n", err, fcntl(g_fd, F_GETLEASE), byte);
	}
	/* Left open, so that nothing is free when the lease is broken and released */
	while (g_exhaust && open("/dev/null", O_RDONLY) >= 0) {
		;
	}
	printf("LEASE_READY pid=%d byte=%" PRIu64 " extents=%u\n", (int)getpid(),
	       first_byte(xal, extents), extents->count);
	fflush(stdout);

	while (!g_release && !g_stop) {
		sigsuspend(orig);
	}
	err = xal_extents_release(xal, inode);
	printf("RELEASED err=%d\n", err);
	fflush(stdout);

	for (int i = 0; i < 600 && !g_stop && !g_exhaust; ++i) {
		err = xal_extents_acquire(xal, inode, &extents);
		if (!err) {
			printf("REACQUIRED byte=%" PRIu64 " extents=%u\n", first_byte(xal, extents),
			       extents->count);
			fflush(stdout);
			xal_extents_release(xal, inode);
			break;
		}
		nanosleep(&retry, NULL);
	}
	if (err && !g_exhaust) {
		fprintf(stderr, "FAILED: re-acquire; err(%d)\n", err);
	}
	err = g_exhaust ? 0 : err;

	while (!g_stop) {
		sigsuspend(orig);
	}

	return err ? 1 : 0;
}

int
main(int argc, char *argv[])
{
	static const struct option longopts[] = {
	    {"probe", no_argument, NULL, 'p'},
	    {"shm", no_argument, NULL, 's'},
	    {"thread", no_argument, NULL, 't'},
	    {"fork", no_argument, NULL, 'f'},
	    {"orphan", no_argument, NULL, 'o'},
	    {"idle", required_argument, NULL, 'i'},
	    {"sweep", no_argument, NULL, 'w'},
	    {"no-recheck", no_argument, NULL, 'r'},
	    {"max", required_argument, NULL, 'm'},
	    {"exhaust", no_argument, NULL, 'x'},
	    {"attach", no_argument, NULL, 'a'},
	    {"attach-rw", no_argument, NULL, 'A'},
	    {"write-own", no_argument, NULL, 'W'},
	    {"map-first", no_argument, NULL, 'M'},
	    {"hold-open", no_argument, NULL, 'H'},
	    {"link", required_argument, NULL, 'L'},
	    {NULL, 0, NULL, 0},
	};
	struct xal_opts opts = {0};
	struct sigaction sa = {0};
	sigset_t block, orig;
	struct xal_inode *inode, *idle = NULL, *link = NULL;
	char *idle_path = NULL, *link_path = NULL;
	struct xal *xal;
	int c, mode = 0, err, ret;

	/* Line by line, so that a test sees what was printed before a crash */
	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		if (c == 'r') {
			opts.lease_no_recheck = 1;
			continue;
		}
		if (c == 'm') {
			opts.lease_max = strtoul(optarg, NULL, 0);
			continue;
		}
		if (c == 'x') {
			g_exhaust = true;
			continue;
		}
		if (c == 'a' || c == 'A') {
			g_attach = c == 'a' ? O_RDONLY : O_RDWR;
			continue;
		}
		if (c == 'W') {
			g_write_own = true;
			continue;
		}
		if (c == 'M') {
			g_map_first = true;
			continue;
		}
		if (c == 'H') {
			g_hold_open = true;
			continue;
		}
		if (c == 'L') {
			link_path = optarg;
			continue;
		}
		if (c == '?' || mode) {
			mode = '?';
			break;
		}
		mode = c;
		if (c == 'i') {
			idle_path = optarg;
		}
	}
	if (mode == '?' || argc - optind != 3 || (g_write_own && g_attach != O_RDWR)) {
		fprintf(stderr,
			"Usage: %s [--no-recheck] [--max <n>] [--exhaust] "
			"[--attach | --attach-rw [--write-own]] [--map-first] [--hold-open] "
			"[--link <file2>] "
			"[--probe | --shm | --thread | --fork | --orphan | --idle <file2> | "
			"--sweep] "
			"<dev_uri> <mountpoint> <file>\n",
			argv[0]);
		return 2;
	}
	argv += optind;

	// Blocked so that only hold(), where it waits for them, and orphan()'s child take these
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigprocmask(SIG_BLOCK, &block, &orig);

	opts.be = XAL_BACKEND_FIEMAP;
	opts.watch_mode = XAL_WATCHMODE_READ_LEASE;
	opts.mountpoint = argv[1];
	if (mode == 's') {
		opts.shm_name = "/xal_lease_hold_test";
	}

	err = xal_open_from_uri(argv[0], &xal, &opts);
	if (mode == 's') {
		printf("OPEN err=%d\n", err);
		if (!err) {
			xal_close(xal);
		}
		return 0;
	}
	if (err) {
		fprintf(stderr, "FAILED: xal_open_from_uri(); err(%d)\n", err);
		return 1;
	}

	err = xal_index(xal);
	if (!err) {
		err = xal_get_inode(xal, argv[2], &inode);
	}
	if (!err && idle_path) {
		err = xal_get_inode(xal, idle_path, &idle);
	}
	if (!err && link_path) {
		err = xal_get_inode(xal, link_path, &link);
	}
	if (err) {
		fprintf(stderr, "FAILED: xal_index() or xal_get_inode(); err(%d)\n", err);
		xal_close(xal);
		return 1;
	}

	if (g_attach >= 0) {
		int open_err, held = -1;

		g_fd = open(argv[2], g_attach | O_CLOEXEC);
		open_err = g_fd < 0 ? -errno : 0;
		if (g_hold_open) {
			held = open(argv[2], O_RDONLY | O_CLOEXEC);
		}
		if (g_map_first) {
			struct xal_extents *extents;

			err = xal_extents_acquire(xal, inode, &extents);
			printf("MAPPED err=%d\n", err ? err : xal_extents_release(xal, inode));
		}
		err = open_err ? open_err : xal_extents_attach_fd(xal, inode, g_fd, NULL);
		printf("ATTACH err=%d lease=%d\n", err, g_fd < 0 ? -1 : fcntl(g_fd, F_GETLEASE));
		if (held >= 0) {
			close(held);
			if (err && !open_err) {
				err = xal_extents_attach_fd(xal, inode, g_fd, NULL);
				printf("ATTACH_RETRY err=%d lease=%d\n", err,
				       fcntl(g_fd, F_GETLEASE));
			}
		}
		if (g_map_first) {
			printf("LEASES=%d\n", count_leases(inode->ino));
		}
		fflush(stdout);
		if (err) {
			xal_close(xal);
			return mode == 'p' ? 0 : 1;
		}
	}

	if (mode == 'p') {
		ret = probe(xal, inode, link);
	} else if (mode == 'f') {
		ret = fork_probe(xal, inode);
	} else if (mode == 'o') {
		ret = orphan(xal, inode);
	} else if (mode == 'w') {
		ret = sweep(xal, inode);
	} else {
		ret = hold(xal, inode, idle, mode == 't', &orig);
	}

	xal_close(xal);
	if (g_fd >= 0) {
		printf("CLOSED lease=%d fd_open=%d\n", fcntl(g_fd, F_GETLEASE),
		       fcntl(g_fd, F_GETFD) >= 0);
		close(g_fd);
	}

	return ret;
}
