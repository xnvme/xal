/**
 * Attaching to a state region published at another version must fail
 * ==================================================================
 *
 * Opens a primary with opts.shm_name over an xNVMe device handle, indexes it, and attaches to the
 * published regions with xal_from_shm() from the same process: the primary keeps them alive, so
 * no second process is needed. The version in the state region is then edited through the primary's
 * own mapping, and the attach retried; -EPROTO is the whole point of the check, and restoring the
 * version shows that the version is what refused it.
 *
 * Usage: test_shm_version <dev_uri> <mountpoint>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <libxnvme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xal.h>

#define SHM_NAME "xal_test_shm_version"

/**
 * Attach once and report whether xal_from_shm() returned what the caller expected
 */
static int
expect_attach(int want, const char *what)
{
	struct xal *secondary;
	int err;

	err = xal_from_shm(SHM_NAME, &secondary);
	if (err != want) {
		printf("FAILED: %s; xal_from_shm() gave err(%d), expected err(%d)\n", what, err,
		       want);
		if (!err) {
			xal_close(secondary);
		}
		return 1;
	}

	if (!err) {
		xal_close(secondary);
	}

	printf("OK: %s\n", what);

	return 0;
}

int
main(int argc, char *argv[])
{
	struct xnvme_opts xnvme_opts = {0};
	struct xal_opts opts = {0};
	struct xnvme_dev *dev;
	struct xal *xal;
	int failures = 0;
	int err;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <dev_uri> <mountpoint>\n", argv[0]);
		return 2;
	}

	xnvme_opts_set_defaults(&xnvme_opts);

	dev = xnvme_dev_open(argv[1], &xnvme_opts);
	if (!dev) {
		fprintf(stderr, "FAILED: xnvme_dev_open(%s); errno(%d)\n", argv[1], errno);
		return 1;
	}

	opts.be = XAL_BACKEND_FIEMAP;
	opts.mountpoint = argv[2];
	opts.shm_name = SHM_NAME;
	opts.watch_mode = XAL_WATCHMODE_DIRTY_DETECTION;

	err = xal_open(dev, &xal, &opts);
	if (err) {
		fprintf(stderr, "FAILED: xal_open(); err(%d)\n", err);
		xnvme_dev_close(dev);
		return 1;
	}

	err = xal_index(xal);
	if (err) {
		fprintf(stderr, "FAILED: xal_index(); err(%d)\n", err);
		xal_close(xal);
		xnvme_dev_close(dev);
		return 1;
	}

	failures += expect_attach(0, "attach to a freshly published index");

	xal->state->version = XAL_SHM_VERSION + 1;
	failures += expect_attach(-EPROTO, "attach to a region published at another version");

	xal->state->version = XAL_SHM_VERSION;
	failures += expect_attach(0, "attach once the version is restored");

	xal_close(xal); // unlinks the regions; the primary owns them
	xnvme_dev_close(dev);

	return failures ? 1 : 0;
}
