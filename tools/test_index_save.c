/**
 * Saving an index must fail when the filesystem changed after its inodes were read
 * ================================================================================
 *
 * Opens the XFS backend on an unmounted device, reads the inodes and indexes them, runs the given
 * shell command, which is to mount the filesystem, change it and unmount it, and then saves the
 * index. The pools are of the state before the command, so xal_index_save() must refuse them
 * with -ESTALE; without a command the same save succeeds.
 *
 * Usage: test_index_save <dev_uri> <index file> [<command>]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <libxnvme.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char *argv[])
{
	struct xnvme_opts xnvme_opts = {0};
	struct xal_opts opts = {0};
	struct xnvme_dev *dev;
	struct xal *xal;
	int want = argc == 4 ? -ESTALE : 0;
	int err;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "Usage: %s <dev_uri> <index file> [<command>]\n", argv[0]);
		return 2;
	}

	xnvme_opts_set_defaults(&xnvme_opts);

	dev = xnvme_dev_open(argv[1], &xnvme_opts);
	if (!dev) {
		fprintf(stderr, "FAILED: xnvme_dev_open(%s); errno(%d)\n", argv[1], errno);
		return 1;
	}

	opts.be = XAL_BACKEND_XFS;

	err = xal_open(dev, &xal, &opts);
	if (!err) {
		err = xal_dinodes_retrieve(xal);
	}
	if (!err) {
		err = xal_index(xal);
	}
	if (err) {
		fprintf(stderr, "FAILED: indexing %s; err(%d)\n", argv[1], err);
		return 1;
	}

	if (argc == 4 && system(argv[3])) {
		fprintf(stderr, "FAILED: %s\n", argv[3]);
		return 1;
	}

	err = xal_index_save(xal, argv[2]);
	printf("xal_index_save(%s); err(%d)\n", argv[2], err);

	xal_close(xal);
	xnvme_dev_close(dev);

	return err != want;
}
