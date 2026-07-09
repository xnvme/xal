/**
 * xal_snapshot_hold -- hold a live reflink snapshot open for inspection
 * =====================================================================
 *
 * Opens the FIEMAP backend in XAL_WATCHMODE_REFLINK_SNAPSHOT, indexes (which reflinks every
 * regular file -- optionally restricted to <subtree> -- into <mountpoint>/.xal_snapshot.<pid>),
 * announces readiness, then blocks until SIGTERM/SIGINT and calls xal_close() (which purges the
 * shadow directory: clears immutable, unlinks the clones, rmdir).
 *
 * This is a test aid: a single xal handle keeps the clones (and their pinned blocks) alive for the
 * lifetime of the process, so a test can inspect the shadow dir, mutate origins to force CoW, and
 * confirm the clones stay put -- the same guarantee homi relies on, without the daemon.
 *
 * Usage: xal_snapshot_hold <dev_uri> <mountpoint> [subtree]
 *
 * On success, one line is printed to stdout once the snapshot exists:
 *   SNAPSHOT_READY pid=<pid> dir=<mountpoint>/.xal_snapshot.<pid>
 * and the process blocks. On open/index failure an error is printed to stderr and the process
 * exits non-zero (so a bogus/missing subtree is observable as a failed run).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libxal.h>
#include <libxnvme.h>

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int signo)
{
	(void)signo;
	g_stop = 1;
}

int
main(int argc, char *argv[])
{
	struct xnvme_opts xnvme_opts = xnvme_opts_default();
	struct xal_opts opts = {0};
	struct sigaction sa = {0};
	struct xnvme_dev *dev;
	struct xal *xal;
	const char *dev_uri, *mountpoint, *subtree;
	int err;

	if (argc < 3 || argc > 4) {
		fprintf(stderr, "Usage: %s <dev_uri> <mountpoint> [subtree]\n", argv[0]);
		return 2;
	}
	dev_uri = argv[1];
	mountpoint = argv[2];
	subtree = (argc == 4) ? argv[3] : NULL;

	xnvme_opts.be = "linux"; // reflink mode targets a mounted fs; match the daemon's backend
	dev = xnvme_dev_open(dev_uri, &xnvme_opts);
	if (!dev) {
		fprintf(stderr, "FAILED: xnvme_dev_open(%s); errno(%d)\n", dev_uri, errno);
		return 1;
	}

	opts.be = XAL_BACKEND_FIEMAP;
	opts.watch_mode = XAL_WATCHMODE_REFLINK_SNAPSHOT;
	opts.mountpoint = mountpoint;
	opts.reflink_subtree = subtree; // NULL => whole indexed tree

	err = xal_open(dev, &xal, &opts);
	if (err < 0) {
		fprintf(stderr, "FAILED: xal_open(); err(%d)\n", err);
		xnvme_dev_close(dev);
		return 1;
	}

	err = xal_dinodes_retrieve(xal); // no-op for FIEMAP; mirrors the daemon's open sequence
	if (err) {
		fprintf(stderr, "FAILED: xal_dinodes_retrieve(); err(%d)\n", err);
		goto fail_close;
	}

	err = xal_index(xal); // creates the snapshot (reflink clones + captured extents)
	if (err) {
		fprintf(stderr, "FAILED: xal_index(); err(%d)\n", err);
		goto fail_close;
	}

	// Install handlers, then block SIGTERM/SIGINT before announcing readiness. Waiting with
	// sigsuspend (which atomically restores the unblocked mask and waits) closes the classic
	// pause() race: a signal arriving between the g_stop test and the wait would set g_stop while
	// blocked and be delivered on the next sigsuspend, rather than being lost with the process
	// stuck sleeping forever.
	sigset_t block, orig;

	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigprocmask(SIG_BLOCK, &block, &orig);

	printf("SNAPSHOT_READY pid=%d dir=%s/.xal_snapshot.%d\n", (int)getpid(), mountpoint,
	       (int)getpid());
	fflush(stdout);

	while (!g_stop) {
		sigsuspend(&orig); // unblock SIGTERM/SIGINT and wait atomically; no missed-signal window
	}

	sigprocmask(SIG_SETMASK, &orig, NULL);

	xal_close(xal); // purges the shadow dir (clears immutable, unlinks, rmdir)
	xnvme_dev_close(dev);

	printf("SNAPSHOT_RELEASED\n");
	fflush(stdout);

	return 0;

fail_close:
	xal_close(xal);
	xnvme_dev_close(dev);
	return 1;
}
