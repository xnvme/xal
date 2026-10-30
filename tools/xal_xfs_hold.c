/**
 * xal_xfs_hold -- keep a device open with the XFS backend
 * =======================================================
 *
 * Opens <dev_uri> with xNVMe and xal_open() with XAL_BACKEND_XFS, prints READY err=<err> and, on
 * success, waits for SIGTERM or SIGINT before closing. This is a test aid: it lets a test check
 * what the XFS backend's claim on a block device keeps others from doing meanwhile.
 *
 * Usage: xal_xfs_hold <dev_uri>
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include <libxal.h>

int
main(int argc, char *argv[])
{
	struct xal_opts opts = {.be = XAL_BACKEND_XFS};
	struct xnvme_opts xnvme_opts = {0};
	struct xnvme_dev *dev;
	struct xal *xal;
	sigset_t set;
	int err, sig;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <dev_uri>\n", argv[0]);
		return 2;
	}

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigprocmask(SIG_BLOCK, &set, NULL);

	xnvme_opts_set_defaults(&xnvme_opts);
	dev = xnvme_dev_open(argv[1], &xnvme_opts);
	if (!dev) {
		printf("READY err=%d\n", -errno);
		return 1;
	}

	err = xal_open(dev, &xal, &opts);
	printf("READY err=%d\n", err);
	fflush(stdout);
	if (!err) {
		sigwait(&set, &sig);
		xal_close(xal);
	}
	xnvme_dev_close(dev);

	return err ? 1 : 0;
}
