/**
 * A reader's xal_mark_dirty() gets an index in XAL_WATCHMODE_NONE re-indexed by xal-server
 * ========================================================================================
 *
 * Attaches to an index xal-server published, creates the given file, marks the index dirty, and
 * waits for the server to re-index: the index must come back clean, at a later sequence number,
 * with the file in it. The file is created only once attached, as an attach succeeds only after
 * the server's first index, which could otherwise pick it up.
 *
 * Given the subtree the server indexes, the re-index is made to fail first: the subtree is moved
 * away before the mark, with the file created in it there, and moved back once a re-index has
 * failed, so that the file shows up only through the server's retry.
 *
 * Usage: test_mark_dirty <shm_name> <path> <timeout_ms> [<subtree>]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * Milliseconds on CLOCK_MONOTONIC
 */
static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Attach, waiting while the primary is still publishing
 */
static int
attach(const char *shm_name, int timeout_ms, struct xal **xal)
{
	uint64_t start = now_ms();
	int err;

	for (;;) {
		err = xal_from_shm(shm_name, xal);
		if ((err != -EAGAIN && err != -ESTALE) ||
		    now_ms() - start >= (uint64_t)timeout_ms) {
			return err;
		}
		usleep(1000);
	}
}

static int
create_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);

	if (fd < 0) {
		return -errno;
	}
	if (write(fd, "x", 1) != 1) {
		close(fd);
		return -EIO;
	}

	return close(fd) ? -errno : 0;
}

/**
 * Move the subtree away with the file in it, mark the index dirty and wait for a re-index to fail
 *
 * A failed re-index leaves the index dirty at a later, even sequence number, which replaces *seq.
 */
static int
fail_reindex(struct xal *xal, const char *subtree, const char *path, int timeout_ms, int *seq)
{
	char away[PATH_MAX], moved[PATH_MAX];
	size_t len = strlen(subtree);
	bool failed = false;
	uint64_t start;

	if (strncmp(path, subtree, len) || path[len] != '/') {
		printf("FAILED: %s is not under %s\n", path, subtree);
		return -EINVAL;
	}
	snprintf(away, sizeof(away), "%s.away", subtree);
	snprintf(moved, sizeof(moved), "%s%s", away, path + len);

	if (rename(subtree, away) || create_file(moved)) {
		printf("FAILED: moving %s away; errno(%d)\n", subtree, errno);
		return -EIO;
	}

	start = now_ms();
	xal_mark_dirty(xal);

	/* A successful index also looks like this for a moment, between its last advance of the
	 * sequence lock and the index going clean; with the subtree away none can succeed. */
	while (now_ms() - start < (uint64_t)timeout_ms) {
		int cur = xal_get_seq_lock(xal);

		if (!(cur & 1) && cur != *seq && xal_is_dirty(xal)) {
			printf("OK: a re-index failed %" PRIu64 " ms after the mark\n",
			       now_ms() - start);
			*seq = cur;
			failed = true;
			break;
		}
		usleep(1000);
	}
	if (rename(away, subtree)) {
		printf("FAILED: moving %s back; errno(%d)\n", subtree, errno);
		return -EIO;
	}

	return failed ? 0 : -ETIMEDOUT;
}

int
main(int argc, char *argv[])
{
	struct xal_inode *inode;
	struct xal *xal;
	int timeout_ms, seq, err;
	uint64_t start;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "Usage: %s <shm_name> <path> <timeout_ms> [<subtree>]\n", argv[0]);
		return 2;
	}
	timeout_ms = atoi(argv[3]);

	err = attach(argv[1], timeout_ms, &xal);
	if (err) {
		printf("FAILED: xal_from_shm(%s); err(%d)\n", argv[1], err);
		return 1;
	}
	seq = xal_get_seq_lock(xal);

	if (argc == 5) {
		err = fail_reindex(xal, argv[4], argv[2], timeout_ms, &seq);
		if (err) {
			printf("FAILED: no failed re-index to retry; err(%d)\n", err);
			xal_close(xal);
			return 1;
		}
	} else {
		err = create_file(argv[2]);
		if (err) {
			printf("FAILED: creating %s; err(%d)\n", argv[2], err);
			xal_close(xal);
			return 1;
		}
		if (!xal_get_inode(xal, argv[2], &inode)) {
			printf("FAILED: %s is in the index before the mark\n", argv[2]);
			xal_close(xal);
			return 1;
		}
		xal_mark_dirty(xal);
	}

	start = now_ms();
	while (now_ms() - start < (uint64_t)timeout_ms) {
		int cur = xal_get_seq_lock(xal);

		if (!(cur & 1) && cur != seq && !xal_is_dirty(xal) &&
		    !xal_get_inode(xal, argv[2], &inode) && cur == xal_get_seq_lock(xal)) {
			printf("OK: %s indexed %" PRIu64 " ms after %s\n", argv[2],
			       now_ms() - start, argc == 5 ? "the subtree came back" : "the mark");
			xal_close(xal);
			return 0;
		}
		usleep(1000);
	}

	printf("FAILED: %s not indexed within %d ms of the mark\n", argv[2], timeout_ms);
	xal_close(xal);

	return 1;
}
