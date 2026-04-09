#define _GNU_SOURCE
#include <errno.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xal.h>
#include <xal_odf.h>

#include "test.h"

#define POOL_INODES 16
#define POOL_EXTENTS 16

static struct xal *
make_xal(struct xal_sb *sb, enum xal_backend type)
{
	struct xal_backend_base *be;
	struct xal *xal;
	int err;

	xal = calloc(1, sizeof(*xal));
	if (!xal) {
		return NULL;
	}

	err = xal_pool_map(&xal->inodes, POOL_INODES, POOL_INODES, sizeof(struct xal_inode), NULL);
	if (err) {
		goto failed;
	}

	err = xal_pool_map(&xal->extents, POOL_EXTENTS, POOL_EXTENTS, sizeof(struct xal_extent),
			   NULL);
	if (err) {
		goto failed;
	}

	xal->sb = *sb;
	xal->index_state = &xal->_index_state_storage;
	xal->seq_lock = &xal->_seq_lock_storage;

	be = (struct xal_backend_base *)&xal->be;
	be->type = type;

	return xal;

failed:
	xal_close(xal);
	return NULL;
}

static int
test_fsbno_offset_fiemap(void)
{
	struct xal_sb sb = {.blocksize = 4096};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	TEST_ASSERT(xal_fsbno_offset(xal, 1) == 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 5) == 5 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_fsbno_offset_xfs_power_of_two(void)
{
	// agblocks == (1 << agblklog), so the offset reduces to fsbno * blocksize
	struct xal_sb sb = {.blocksize = 4096, .agblocks = 8, .agblklog = 3};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);

	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	TEST_ASSERT(xal_fsbno_offset(xal, 3) == 3 * 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 8) == 8 * 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 11) == 11 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_fsbno_offset_xfs_non_power_of_two(void)
{
	// agblocks=6 with an AG stride of (1 << agblklog)=8: AG boundaries are not multiples of
	// agblocks
	struct xal_sb sb = {.blocksize = 4096, .agblocks = 6, .agblklog = 3};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);

	// fsbno=0: ag=0, bno=0, offset=0
	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	// fsbno=5: ag=0, bno=5, offset=5*4096
	TEST_ASSERT(xal_fsbno_offset(xal, 5) == 5 * 4096);
	// fsbno=8: ag=1, bno=0, offset=(1*6+0)*4096=24576, not 8*4096=32768
	TEST_ASSERT(xal_fsbno_offset(xal, 8) == 6 * 4096);
	// fsbno=9: ag=1, bno=1, offset=(1*6+1)*4096=28672
	TEST_ASSERT(xal_fsbno_offset(xal, 9) == 7 * 4096);
	// fsbno=16: ag=2, bno=0, offset=(2*6+0)*4096=49152, not 16*4096=65536
	TEST_ASSERT(xal_fsbno_offset(xal, 16) == 12 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_bytes(void)
{
	struct xal_sb sb = {.blocksize = 4096};
	struct xal_extent extent = {.start_offset = 2, .start_block = 10, .nblocks = 4};
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_bytes(xal, &extent, &out);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(out.unit == XAL_EXTENT_UNIT_BYTES);
	TEST_ASSERT(out.start_offset == 2 * 4096);
	TEST_ASSERT(out.size == 4 * 4096);
	// FIEMAP backend: xal_fsbno_offset = start_block * blocksize
	TEST_ASSERT(out.start_block == 10 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_bytes_null(void)
{
	struct xal_sb sb = {.blocksize = 4096};
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_bytes(xal, NULL, &out);
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_lba(void)
{
	struct xal_sb sb = {.blocksize = 4096, .lba_blksze = 512};
	struct xal_extent extent = {.start_offset = 2, .start_block = 10, .nblocks = 4};
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_lba(xal, &extent, &out);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(out.unit == XAL_EXTENT_UNIT_LBA);
	TEST_ASSERT(out.start_offset == (2 * 4096) / 512);
	TEST_ASSERT(out.size == (4 * 4096) / 512);
	TEST_ASSERT(out.start_block == (10 * 4096) / 512);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_lba_null(void)
{
	struct xal_sb sb = {.blocksize = 4096, .lba_blksze = 512};
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_lba(xal, NULL, &out);
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
test_walk_dirty_returns_estale(void)
{
	struct xal_sb sb = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	xal_mark_dirty(xal);

	err = xal_walk(xal, xal_inode_at(xal, 0), NULL, NULL);
	TEST_ASSERT(err == -ESTALE);

	xal_close(xal);
	return 0;
}

int
main(void)
{
	int failures = 0;

	TEST_RUN(test_fsbno_offset_fiemap);
	TEST_RUN(test_fsbno_offset_xfs_power_of_two);
	TEST_RUN(test_fsbno_offset_xfs_non_power_of_two);
	TEST_RUN(test_extent_in_bytes);
	TEST_RUN(test_extent_in_bytes_null);
	TEST_RUN(test_extent_in_lba);
	TEST_RUN(test_extent_in_lba_null);
	TEST_RUN(test_walk_dirty_returns_estale);

	return failures;
}
