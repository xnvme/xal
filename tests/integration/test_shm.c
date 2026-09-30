#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

// Shares an index between a primary opened with opts.shm_name on a mounted device and
// secondaries from xal_from_shm(). Usage: xal_test_shm <dev_uri>

static const char *g_dev_uri;

struct compare_args {
	struct xal *secondary;
	uint64_t nfiles;
	uint64_t ndirs;
};

static void
shm_cleanup(const char *shm_name)
{
	char name[128];

	snprintf(name, sizeof(name), "%s_inodes", shm_name);
	shm_unlink(name);
	snprintf(name, sizeof(name), "%s_extents", shm_name);
	shm_unlink(name);
	snprintf(name, sizeof(name), "%s_state", shm_name);
	shm_unlink(name);
}

static int
open_primary(const char *shm_name, struct xal **xal)
{
	struct xal_opts opts = {0};
	int err;

	opts.shm_name = shm_name;

	err = xal_open_from_uri(g_dev_uri, xal, &opts);
	if (err) {
		return err;
	}

	err = xal_index(*xal);
	if (err) {
		xal_close(*xal);
		return err;
	}

	return 0;
}

static int
compare_extents(struct xal *xal, struct xal_inode *inode, struct xal *secondary, char *path)
{
	struct xal_dentries *dentries;
	struct xal_extents *got;
	int err;

	err = xal_get_extents(secondary, path, &got);
	if (err) {
		fprintf(stderr, "FAIL xal_get_extents(%s) on secondary; err(%d)\n", path, err);
		return err;
	}

	if (got->count != inode->content.extents.count) {
		fprintf(stderr, "FAIL %s: %u extents on secondary, %u on primary\n", path,
			got->count, inode->content.extents.count);
		return -EINVAL;
	}

	for (uint32_t i = 0; i < got->count; i++) {
		struct xal_extent *expected =
		    xal_extent_at(xal, inode->content.extents.extent_idx + i);
		struct xal_extent *extent = xal_extent_at(secondary, got->extent_idx + i);

		if (memcmp(expected, extent, sizeof(*extent))) {
			fprintf(stderr, "FAIL %s: extent %u differs\n", path, i);
			return -EINVAL;
		}
	}

	err = xal_get_dentries(secondary, path, &dentries);
	if (err != -ENOTDIR) {
		fprintf(stderr, "FAIL xal_get_dentries(%s) on a file; err(%d)\n", path, err);
		return -EINVAL;
	}

	return 0;
}

static int
compare_dentries(struct xal *xal, struct xal_inode *inode, struct xal *secondary, char *path)
{
	struct xal_extents *extents;
	struct xal_dentries *got;
	int err;

	err = xal_get_dentries(secondary, path, &got);
	if (err) {
		fprintf(stderr, "FAIL xal_get_dentries(%s) on secondary; err(%d)\n", path, err);
		return err;
	}

	if (got->count != inode->content.dentries.count) {
		fprintf(stderr, "FAIL %s: %u entries on secondary, %u on primary\n", path,
			got->count, inode->content.dentries.count);
		return -EINVAL;
	}

	for (uint32_t i = 0; i < got->count; i++) {
		struct xal_inode *expected =
		    xal_inode_at(xal, inode->content.dentries.inodes_idx + i);
		struct xal_inode *child = xal_inode_at(secondary, got->inodes_idx + i);

		if (strcmp(expected->name, child->name)) {
			fprintf(stderr, "FAIL %s: entry %u is %s on secondary, %s on primary\n",
				path, i, child->name, expected->name);
			return -EINVAL;
		}
	}

	err = xal_get_extents(secondary, path, &extents);
	if (err != -EINVAL) {
		fprintf(stderr, "FAIL xal_get_extents(%s) on a directory; err(%d)\n", path, err);
		return -EINVAL;
	}

	return 0;
}

// Look up every entry of the primary in the secondary and compare its extents or entries
static int
compare_cb(struct xal *xal, struct xal_inode *inode, void *cb_args, int XAL_UNUSED(level))
{
	char path[XAL_INODE_PATH_MAXLEN + 1];
	struct compare_args *args = cb_args;
	int err;

	// The root has no path of its own to look up below the mountpoint
	if (inode->parent_idx == XAL_POOL_IDX_NONE) {
		return 0;
	}

	err = xal_inode_path(xal, inode, path, sizeof(path));
	if (err < 0) {
		fprintf(stderr, "FAIL xal_inode_path(%s); err(%d)\n", inode->name, err);
		return err;
	}

	if (xal_inode_is_file(inode)) {
		err = compare_extents(xal, inode, args->secondary, path);
		args->nfiles++;
	} else if (xal_inode_is_dir(inode)) {
		err = compare_dentries(xal, inode, args->secondary, path);
		args->ndirs++;
	} else {
		return 0;
	}

	return err;
}

static int
child_handoff(struct xal *primary, const char *shm_name)
{
	struct compare_args args = {0};
	struct xal *secondary;
	int err;

	err = xal_from_shm(shm_name, &secondary);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(!xal_is_dirty(secondary));

	err = xal_index(secondary);
	TEST_ASSERT(err == -EINVAL);

	args.secondary = secondary;
	err = xal_walk(primary, xal_get_root(primary), compare_cb, &args);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(args.nfiles > 0);
	TEST_ASSERT(args.ndirs > 0);

	// Again through the hash map, the way a secondary gets constant-time lookups
	err = xal_build_lookup_hashmap(secondary);
	TEST_ASSERT(err == 0);

	args.nfiles = 0;
	args.ndirs = 0;
	err = xal_walk(primary, xal_get_root(primary), compare_cb, &args);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(args.nfiles > 0);
	TEST_ASSERT(args.ndirs > 0);

	xal_close(secondary);

	return 0;
}

// A secondary in another process sees the primary's files, extents and entries, by traversal
// and through xal_build_lookup_hashmap(), and cannot index
static int
test_handoff(void)
{
	const char *shm_name = "xal_test_shm_handoff";
	struct xal *primary;
	int status;
	pid_t pid;
	int err;

	shm_cleanup(shm_name);

	err = open_primary(shm_name, &primary);
	TEST_ASSERT(err == 0);

	pid = fork();
	TEST_ASSERT(pid >= 0);

	// The child shares the primary's mappings but must not close it, as that unlinks them
	if (pid == 0) {
		err = child_handoff(primary, shm_name);
		_exit(err ? EXIT_FAILURE : EXIT_SUCCESS);
	}

	err = waitpid(pid, &status, 0);
	xal_close(primary);
	TEST_ASSERT(err == pid);
	TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);

	return 0;
}

// A second primary under a name in use fails with -EEXIST and leaves the first one intact
static int
test_name_taken(void)
{
	const char *shm_name = "xal_test_shm_taken";
	struct xal *secondary;
	struct xal *primary;
	struct xal *second;
	int err;

	shm_cleanup(shm_name);

	err = open_primary(shm_name, &primary);
	TEST_ASSERT(err == 0);

	err = open_primary(shm_name, &second);
	TEST_ASSERT(err == -EEXIST);

	err = xal_from_shm(shm_name, &secondary);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(!xal_is_dirty(secondary));

	xal_close(secondary);
	xal_close(primary);

	return 0;
}

// Marking the primary dirty is seen by an attached secondary
static int
test_mark_dirty(void)
{
	const char *shm_name = "xal_test_shm_dirty";
	struct xal *secondary;
	struct xal *primary;
	int err;

	shm_cleanup(shm_name);

	err = open_primary(shm_name, &primary);
	TEST_ASSERT(err == 0);

	err = xal_from_shm(shm_name, &secondary);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(!xal_is_dirty(secondary));

	xal_mark_dirty(primary);
	TEST_ASSERT(xal_is_dirty(secondary));

	err = xal_walk(secondary, xal_get_root(secondary), NULL, NULL);
	TEST_ASSERT(err == -ESTALE);

	xal_close(secondary);
	xal_close(primary);

	return 0;
}

// Closing the primary marks an attached secondary stale and unlinks the regions
static int
test_primary_close(void)
{
	const char *shm_name = "xal_test_shm_close";
	struct xal *secondary;
	struct xal *primary;
	struct xal *late;
	int err;

	shm_cleanup(shm_name);

	err = open_primary(shm_name, &primary);
	TEST_ASSERT(err == 0);

	err = xal_from_shm(shm_name, &secondary);
	TEST_ASSERT(err == 0);

	xal_close(primary);

	TEST_ASSERT(xal_is_dirty(secondary));

	err = xal_walk(secondary, xal_get_root(secondary), NULL, NULL);
	TEST_ASSERT(err == -ESTALE);

	err = xal_from_shm(shm_name, &late);
	TEST_ASSERT(err == -ENOENT);

	xal_close(secondary);

	return 0;
}

int
main(int argc, char *argv[])
{
	int failures = 0;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <dev_uri>\n", argv[0]);
		return 2;
	}

	g_dev_uri = argv[1];

	TEST_RUN(test_handoff);
	TEST_RUN(test_name_taken);
	TEST_RUN(test_mark_dirty);
	TEST_RUN(test_primary_close);

	return failures;
}
