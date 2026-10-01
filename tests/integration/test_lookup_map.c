#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>

#include "test.h"

// Lookups by hash map, built hash map and traversal follow a change to the tree on re-index.
// Usage: xal_test_lookup_map [basedir], where basedir defaults to /tmp

static const char *g_basedir = "/tmp";

struct tree {
	char root[256];
	char a[300];
	char b[300];
	char bc[300];
	char d[300];
	char de[300];
};

static int
tree_init(struct tree *tree)
{
	int err;

	snprintf(tree->root, sizeof(tree->root), "%s/xal_lookup_map_XXXXXX", g_basedir);
	TEST_ASSERT(mkdtemp(tree->root) != NULL);

	snprintf(tree->a, sizeof(tree->a), "%s/a", tree->root);
	snprintf(tree->b, sizeof(tree->b), "%s/b", tree->root);
	snprintf(tree->bc, sizeof(tree->bc), "%s/b/c", tree->root);
	snprintf(tree->d, sizeof(tree->d), "%s/d", tree->root);
	snprintf(tree->de, sizeof(tree->de), "%s/d/e", tree->root);

	err = mkdir(tree->a, 0755);
	TEST_ASSERT(err == 0);
	err = mkdir(tree->b, 0755);
	TEST_ASSERT(err == 0);
	err = mkdir(tree->bc, 0755);
	TEST_ASSERT(err == 0);

	return 0;
}

// Replace b/c with d/e
static int
tree_change(struct tree *tree)
{
	int err;

	err = rmdir(tree->bc);
	TEST_ASSERT(err == 0);
	err = mkdir(tree->d, 0755);
	TEST_ASSERT(err == 0);
	err = mkdir(tree->de, 0755);
	TEST_ASSERT(err == 0);

	return 0;
}

static void
tree_remove(struct tree *tree)
{
	rmdir(tree->de);
	rmdir(tree->d);
	rmdir(tree->bc);
	rmdir(tree->b);
	rmdir(tree->a);
	rmdir(tree->root);
}

static int
open_and_index(struct tree *tree, enum xal_file_lookupmode mode, struct xal **xal)
{
	struct xal_opts opts = {0};
	int err;

	opts.file_lookupmode = mode;

	err = xal_be_fiemap_open(xal, tree->root, &opts);
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
expect_found(struct xal *xal, char *path)
{
	char found[XAL_INODE_PATH_MAXLEN + 1];
	struct xal_inode *inode;
	int err;

	err = xal_get_inode(xal, path, &inode);
	if (err) {
		fprintf(stderr, "FAIL xal_get_inode(%s); err(%d)\n", path, err);
		return err;
	}

	err = xal_inode_path(xal, inode, found, sizeof(found));
	TEST_ASSERT(err >= 0);

	if (strcmp(found, path)) {
		fprintf(stderr, "FAIL xal_get_inode(%s) returned the inode at %s\n", path, found);
		return -EINVAL;
	}

	return 0;
}

static int
expect_missing(struct xal *xal, char *path)
{
	struct xal_inode *inode;
	int err;

	err = xal_get_inode(xal, path, &inode);
	if (err != -ENOENT) {
		fprintf(stderr, "FAIL xal_get_inode(%s) of a removed path; err(%d)\n", path, err);
		return -EINVAL;
	}

	return 0;
}

static int
check_reindex(struct xal *xal, struct tree *tree)
{
	int err;

	err = expect_found(xal, tree->a);
	TEST_ASSERT(err == 0);
	err = expect_found(xal, tree->bc);
	TEST_ASSERT(err == 0);

	err = tree_change(tree);
	TEST_ASSERT(err == 0);

	err = xal_index(xal);
	TEST_ASSERT(err == 0);

	err = expect_found(xal, tree->a);
	TEST_ASSERT(err == 0);
	err = expect_found(xal, tree->b);
	TEST_ASSERT(err == 0);
	err = expect_found(xal, tree->de);
	TEST_ASSERT(err == 0);
	err = expect_missing(xal, tree->bc);
	TEST_ASSERT(err == 0);

	return 0;
}

static int
test_reindex_lookupmode_hashmap(void)
{
	struct tree tree;
	struct xal *xal;
	int err;

	err = tree_init(&tree);
	TEST_ASSERT(err == 0);

	err = open_and_index(&tree, XAL_FILE_LOOKUPMODE_HASHMAP, &xal);
	TEST_ASSERT(err == 0);

	err = check_reindex(xal, &tree);

	xal_close(xal);
	tree_remove(&tree);
	TEST_ASSERT(err == 0);

	return 0;
}

static int
test_reindex_built_hashmap(void)
{
	struct tree tree;
	struct xal *xal;
	int err;

	err = tree_init(&tree);
	TEST_ASSERT(err == 0);

	err = open_and_index(&tree, XAL_FILE_LOOKUPMODE_TRAVERSE, &xal);
	TEST_ASSERT(err == 0);

	err = xal_build_lookup_hashmap(xal);
	if (!err) {
		err = check_reindex(xal, &tree);
	}

	xal_close(xal);
	tree_remove(&tree);
	TEST_ASSERT(err == 0);

	return 0;
}

static int
test_reindex_traverse(void)
{
	struct tree tree;
	struct xal *xal;
	int err;

	err = tree_init(&tree);
	TEST_ASSERT(err == 0);

	err = open_and_index(&tree, XAL_FILE_LOOKUPMODE_TRAVERSE, &xal);
	TEST_ASSERT(err == 0);

	err = check_reindex(xal, &tree);

	xal_close(xal);
	tree_remove(&tree);
	TEST_ASSERT(err == 0);

	return 0;
}

int
main(int argc, char *argv[])
{
	int failures = 0;

	if (argc > 1) {
		g_basedir = argv[1];
	}

	TEST_RUN(test_reindex_lookupmode_hashmap);
	TEST_RUN(test_reindex_built_hashmap);
	TEST_RUN(test_reindex_traverse);

	return failures;
}
