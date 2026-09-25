#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>

#include "test.h"

// Uses xal_be_fiemap_open() on a directory of only subdirectories, so FIEMAP is never called.
// Usage: xal_test_inotify [basedir], where basedir defaults to /tmp

#define POLL_TIMEOUT_MS 200

static const char *g_basedir = "/tmp";

// Wait up to timeout_ms milliseconds for xal to become dirty
static bool
poll_until_dirty(struct xal *xal, int timeout_ms)
{
	int i;

	for (i = 0; i < timeout_ms; i++) {
		if (xal_is_dirty(xal)) {
			return true;
		}
		usleep(1000);
	}

	return false;
}

// Creating a file in the watched directory must mark xal dirty
static int
test_dirty_detection_on_create(void)
{
	char tmpdir[256];
	char newfile[256];
	struct xal_opts opts = {.watch_mode = XAL_WATCHMODE_REFLINK_SNAPSHOT};
	struct xal *xal;
	int fd, err;

	snprintf(tmpdir, sizeof(tmpdir), "%s/xal_inotify_XXXXXX", g_basedir);
	TEST_ASSERT(mkdtemp(tmpdir) != NULL);

	// tmpdir is empty: xal_index will not call FIEMAP on any files
	err = xal_be_fiemap_open(&xal, tmpdir, &opts);
	TEST_ASSERT(err == 0);

	err = xal_index(xal);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	// Trigger IN_CREATE
	snprintf(newfile, sizeof(newfile), "%s/trigger", tmpdir);
	fd = open(newfile, O_CREAT | O_WRONLY, 0644);
	TEST_ASSERT(fd >= 0);
	close(fd);

	TEST_ASSERT(poll_until_dirty(xal, POLL_TIMEOUT_MS));

	unlink(newfile);
	xal_close(xal);
	rmdir(tmpdir);

	return 0;
}

// Removing a subdirectory present during indexing must mark xal dirty
static int
test_dirty_detection_on_delete(void)
{
	char tmpdir[256];
	char subdir[256];
	struct xal_opts opts = {.watch_mode = XAL_WATCHMODE_REFLINK_SNAPSHOT};
	struct xal *xal;
	int err;

	snprintf(tmpdir, sizeof(tmpdir), "%s/xal_inotify_XXXXXX", g_basedir);
	TEST_ASSERT(mkdtemp(tmpdir) != NULL);

	// A subdirectory gives xal_index something to walk without touching files
	snprintf(subdir, sizeof(subdir), "%s/subdir", tmpdir);
	err = mkdir(subdir, 0755);
	TEST_ASSERT(err == 0);

	err = xal_be_fiemap_open(&xal, tmpdir, &opts);
	TEST_ASSERT(err == 0);

	err = xal_index(xal);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	// Trigger IN_DELETE
	err = rmdir(subdir);
	TEST_ASSERT(err == 0);

	TEST_ASSERT(poll_until_dirty(xal, POLL_TIMEOUT_MS));

	xal_close(xal);
	rmdir(tmpdir);

	return 0;
}

int
main(int argc, char *argv[])
{
	int failures = 0;

	if (argc > 1) {
		g_basedir = argv[1];
	}

	TEST_RUN(test_dirty_detection_on_create);
	TEST_RUN(test_dirty_detection_on_delete);

	return failures;
}
