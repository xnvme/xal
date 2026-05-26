#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_xfs.h>
#include <xal_odf.h>

struct pair_u64 {
	uint64_t l0;
	uint64_t l1;
};

KHASH_MAP_INIT_INT64(ino_to_dinode, struct xal_odf_dinode *);

static int
decode_dentry(void *buf, struct xal_inode *dentry);

static int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index);

static int
process_ino(struct xal *xal, uint64_t ino, struct xal_inode *self);

int
xal_be_xfs_index(struct xal *xal);

static int
dev_read(struct xnvme_dev *dev, void *buf, size_t count, uint64_t offset)
{
	struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);
	const struct xnvme_geo *geo = xnvme_dev_get_geo(dev);
	int err;

	if (count > geo->mdts_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- count(%zu) > mdts_nbytes(%" PRIu32 ")", count,
			  geo->mdts_nbytes);
		return -EINVAL;
	}
	if (count % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- unaligned count(%zu);", count);
		return -EINVAL;
	}
	if (offset % geo->lba_nbytes) {
		XAL_DEBUG("FAILED: dev_read(...) -- unaligned offset(%zu);", offset);
		return -EINVAL;
	}

	memset(buf, 0, count);

	err = xnvme_nvm_read(&ctx, xnvme_dev_get_nsid(dev), offset / geo->lba_nbytes,
			     (count / geo->lba_nbytes) - 1, buf, NULL);
	if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
		XAL_DEBUG("FAILED: xnvme_nvm_read(...):err(%d)", err);
		return -EIO;
	}

	return 0;
}

static int
dev_read_into(struct xnvme_dev *dev, void *iobuf, size_t count, off_t offset, void *buf)
{
	int err;

	err = dev_read(dev, iobuf, count, offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read_into(); err(%d)", err);
		return err;
	}

	memcpy(buf, iobuf, count);

	return 0;
}

static int
compare_inode(const void *a, const void *b)
{
	const struct xal_inode *ia = (const struct xal_inode *)a;
	const struct xal_inode *ib = (const struct xal_inode *)b;
	return strcmp(ia->name, ib->name);
}

static __attribute__((unused)) uint32_t
ino_abs_to_rel(struct xal *xal, uint64_t inoabs)
{
	return inoabs & ((1ULL << (xal->sb.agblklog + xal->sb.inopblog)) - 1);
}

/**
 * Decodes the given inode number in AG-Relative Inode number format
 *
 * For details on the format then see the description in section "13.3.1 Inode Numbers"
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 * @param agbno Pointer to store the AG-relative block number
 * @param agbino The inode relative to the AG-relative block
 */
static void
xal_ino_decode_relative(struct xal *xal, uint32_t ino, uint64_t *agbno, uint32_t *agbino)
{
	// Block Number relative to Allocation Group
	*agbno = (ino >> xal->sb.inopblog) & ((1ULL << xal->sb.agblklog) - 1);

	// Inode number relative to Block
	*agbino = ino & ((1ULL << xal->sb.inopblog) - 1);
}

/**
 * Decodes the inode number in Absolute Inode number format
 *
 * For details on the format then see the description in section "13.3.1 Inode Numbers"
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 * @param seqno Pointer to store the AG number
 * @param agbno Pointer to store the AG-relative block number
 * @param agbino The inode relative to the AG-relative block
 */
static void
xal_ino_decode_absolute(struct xal *xal, uint64_t ino, uint32_t *seqno, uint64_t *agbno,
			uint32_t *agbino)
{
	// Allocation Group Number -- represented usually stored in 'ag.seqno'
	*seqno = ino >> (xal->sb.inopblog + xal->sb.agblklog);

	xal_ino_decode_relative(xal, ino, agbno, agbino);
}

/**
 * Compute the absolute disk offset of the given 'agbno' relative to the ag with the given 'seqno'
 *
 * Relative block numbers are utilized in e.g. B+Tree sibilings and pointers when they only ever
 * refer to blocks within a given allocation group, for example the B+Tree tracking all allocated
 * inodes, the inode-allocation-btree.
 * Generally, then they are used in the btree-short-form, whereas the btree-long-form has the seqno
 * encoded in the block number.
 */
static uint64_t
xal_agbno_absolute_offset(struct xal *xal, uint32_t seqno, uint64_t agbno)
{
	// Absolute Inode offset in bytes
	return (seqno * (uint64_t)xal->sb.agblocks + agbno) * xal->sb.blocksize;
}

/**
 * Compute the byte-offset on disk of the given inode in absolute inode number format
 *
 * @param xal Pointer to the xal-instance that the inode belongs to
 * @param ino The absolute inode number to decode
 *
 * @returns The byte-offset on success.
 */
static __attribute__((unused)) uint64_t
xal_ino_decode_absolute_offset(struct xal *xal, uint64_t ino)
{
	uint32_t seqno, agbino;
	uint64_t offset, agbno;

	xal_ino_decode_absolute(xal, ino, &seqno, &agbno, &agbino);

	// Absolute Inode offset in bytes
	offset = (seqno * (uint64_t)xal->sb.agblocks + agbno) * xal->sb.blocksize;

	return offset + ((uint64_t)agbino * xal->sb.inodesize);
}

void
xal_be_xfs_close(struct xal *xal)
{
	struct xal_be_xfs *be;

	if (!xal) {
		return;
	}

	be = (struct xal_be_xfs *)xal->be;

	xnvme_buf_free(xal->dev, be->buf);
	kh_destroy(ino_to_dinode, be->dinodes_map);
	free(be->dinodes);
}

/**
 * Derive the values needed to decode the records of a btree-root-node embedded in a dinode
 *
 * @param xal The xal instance
 * @param dinode The dinode in question
 * @param maxrecs Optional Maximum number of records in the dinode
 * @param keys Optional pointer to store dinode-offset to keys
 * @param pointers Optional pointer to store dinode-offset to pointers
 */
static void
btree_dinode_meta(struct xal *xal, struct xal_odf_dinode *dinode, size_t *maxrecs, size_t *keys,
		  size_t *pointers)
{
	size_t core_nbytes = sizeof(*dinode);
	size_t attr_ofz = core_nbytes + dinode->di_forkoff * 8UL;
	size_t attr_nbytes = xal->sb.inodesize - attr_ofz;
	size_t data_nbytes = xal->sb.inodesize - core_nbytes - attr_nbytes;
	size_t mrecs = (data_nbytes - 4) / 16;

	XAL_DEBUG("di_forkoff(%" PRIu16 ", %" PRIu64 ")", dinode->di_forkoff,
		  dinode->di_forkoff * 8UL);

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = core_nbytes + 2 + 2;
	}
	if (pointers) {
		*pointers = core_nbytes + 2 + 2 + mrecs * 8;
	}
}

/**
 * Compute max records in a long-format btree block with key/pointer offsets
 */
static void
btree_lblock_meta(struct xal *xal, size_t *maxrecs, size_t *keys, size_t *pointers)
{
	size_t hdr_nbytes = sizeof(struct xal_odf_btree_lfmt);
	size_t mrecs = (xal->sb.blocksize - hdr_nbytes) / 16;

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = hdr_nbytes;
	}
	if (pointers) {
		*pointers = hdr_nbytes + mrecs * 8;
	}
}

/**
 * Compute max records in a short-format btree block with key/pointer offsets
 */
static void
btree_sblock_meta(struct xal *xal, size_t *maxrecs, size_t *keys, size_t *pointers)
{
	size_t hdr_nbytes = sizeof(struct xal_odf_btree_sfmt);
	size_t mrecs = (xal->sb.blocksize - hdr_nbytes) / 8;

	if (maxrecs) {
		*maxrecs = mrecs;
	}
	if (keys) {
		*keys = hdr_nbytes;
	}
	if (pointers) {
		*pointers = hdr_nbytes + mrecs * 4;
	}
}

/**
 * Retrieve the IAB3 block 'blkno' in 'ag' via 'xal->be.buf' into 'buf' and convert endianess
 */
static int
read_iab3_block(struct xal *xal, struct xal_ag *ag, uint64_t blkno, void *buf)
{
	uint64_t ofz = xal_agbno_absolute_offset(xal, ag->seqno, blkno);
	struct xal_odf_btree_sfmt *block = (void *)buf;
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	int err;

	XAL_DEBUG("ENTER: blkno(0x%" PRIx64 ", %" PRIu64 ") @ ofz(%" PRIu64 ")", blkno, blkno, ofz);

	err = dev_read_into(xal->dev, be->buf, xal->sb.blocksize, ofz, buf);
	if (err) {
		XAL_DEBUG("FAILED: dev_read_into(); err(%d)", err);
		return err;
	}

	if (XAL_ODF_IBT_CRC_MAGIC != be32toh(block->magic.num)) {
		XAL_DEBUG("FAILED: expected magic(IAB3) got magic('%.4s', 0x%" PRIx32 "); ",
			  block->magic.text, block->magic.num);
		return -EINVAL;
	}

	block->pos.level = be16toh(block->pos.level);
	block->pos.numrecs = be16toh(block->pos.numrecs);
	block->siblings.left = be32toh(block->siblings.left);
	block->siblings.right = be32toh(block->siblings.right);
	block->blkno = be64toh(block->blkno);

	XAL_DEBUG("INFO:    seqno(%" PRIu32 ")", ag->seqno);
	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", block->magic.text, block->magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", block->pos.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", block->pos.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%08" PRIx32 " @ %" PRIu64 ")", block->siblings.left,
		  xal_agbno_absolute_offset(xal, ag->seqno, block->siblings.left));
	XAL_DEBUG("INFO:      bno(0x%08" PRIx64 " @ %" PRIu64 ")", blkno,
		  xal_agbno_absolute_offset(xal, ag->seqno, blkno));
	XAL_DEBUG("INFO: rightsib(0x%08" PRIx32 " @ %" PRIu64 ")", block->siblings.right,
		  xal_agbno_absolute_offset(xal, ag->seqno, block->siblings.right));

	XAL_DEBUG("EXIT");

	return 0;
}

static int
decode_iab3_leaf_records(struct xal *xal, struct xal_ag *ag, void *buf, uint64_t *index)
{
	struct xal_odf_btree_sfmt *root = (void *)buf;
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	int err = 0;

	XAL_DEBUG("ENTER");

	for (uint16_t reci = 0; reci < root->pos.numrecs; ++reci) {
		uint8_t inodechunk[BUF_NBYTES] = {0};
		struct xal_odf_inobt_rec *rec;
		uint32_t agbino;
		uint64_t agbno;

		rec = (void *)(((uint8_t *)buf) + sizeof(*root) + reci * sizeof(*rec));
		rec->startino = be32toh(rec->startino);
		rec->holemask = be16toh(rec->holemask);
		rec->count = rec->count;
		rec->freecount = rec->freecount;
		rec->free = be64toh(rec->free);

		/**
		 * Determine the block number relative to the allocation group
		 *
		 * This block should be the first block
		 */
		xal_ino_decode_relative(xal, rec->startino, &agbno, &agbino);

		/**
		 * Assumption: if the inode-offset is non-zero, then offset-calucations are
		 *             incorrect as they do not account for the only the block where the
		 *             inode-chunk is supposed to start.
		 */
		assert(agbino == 0);

		/**
		 * Populate the inode-buffer with data from all the blocks
		 */
		{
			uint64_t chunk_nbytes =
			    (CHUNK_NINO / xal->sb.inopblock) * xal->sb.blocksize;
			uint64_t chunk_offset = agbno * xal->sb.blocksize + ag->offset;

			assert(chunk_nbytes < BUF_NBYTES);

			memset(be->buf, 0, chunk_nbytes);

			err = dev_read(xal->dev, be->buf, chunk_nbytes, chunk_offset);
			if (err) {
				XAL_DEBUG("FAILED: dev_read(chunk)");
				return err;
			}
			memcpy(inodechunk, be->buf, chunk_nbytes);
		}

		/**
		 * Traverse the inodes in the chunk, skipping unused and free inodes.
		 */
		for (uint8_t chunk_index = 0; chunk_index < rec->count; ++chunk_index) {
			uint8_t *chunk_cursor = &inodechunk[chunk_index * xal->sb.inodesize];
			uint64_t is_unused = (rec->holemask & (1ULL << chunk_index)) >> chunk_index;
			uint64_t is_free = (rec->free & (1ULL << chunk_index)) >> chunk_index;
			khash_t(ino_to_dinode) *dinodes_map = be->dinodes_map;
			struct xal_odf_dinode *dinode;
			khiter_t iter;

			if (is_unused || is_free) {
				continue;
			}

			dinode = (void *)&be->dinodes[*index * xal->sb.inodesize];
			memcpy(dinode, (void *)chunk_cursor, xal->sb.inodesize);

			iter = kh_put(ino_to_dinode, dinodes_map, be64toh(dinode->ino), &err);
			if (err < 0) {
				XAL_DEBUG("FAILED: kh_put()");
				return -EIO;
			}
			kh_value(dinodes_map, iter) = dinode;

			*index += 1;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Decodes the node and invokes retrieve_dinodes_via_iab3() for each decoded record.
 */
static int
decode_iab3_node_records(struct xal *xal, struct xal_ag *ag, void *buf, uint64_t *index)
{
	uint32_t pointers[ODF_BLOCK_FS_BYTES_MAX / sizeof(uint32_t)] = {0};
	struct xal_odf_btree_sfmt *node = (void *)buf;
	uint8_t *cursor = buf;
	size_t pointers_ofz;
	int err;

	XAL_DEBUG("ENTER");

	btree_sblock_meta(xal, NULL, NULL, &pointers_ofz);

	memcpy(&pointers, cursor + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < node->pos.numrecs; ++rec) {
		uint32_t blkno = be32toh(pointers[rec]);

		XAL_DEBUG("INFO: ptr[%" PRIu16 "] = 0x%" PRIx32, rec, blkno);
		err = retrieve_dinodes_via_iab3(xal, ag, blkno, index);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3() : err(%d)", err);
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Retrieve all the allocated inodes stored within the given allocation group
 *
 * It is assumed that the inode-allocation-b+tree is rooted at the given 'blkno'
 */
static int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, uint64_t *index)
{
	uint8_t block[ODF_BLOCK_FS_BYTES_MAX] = {0};
	struct xal_odf_btree_sfmt *node = (void *)block;
	int err;

	XAL_DEBUG("ENTER");
	XAL_DEBUG("INFO: seqno(%" PRIu32 "), blkno(0x%" PRIx64 ")", ag->seqno, blkno);

	err = read_iab3_block(xal, ag, blkno, block);
	if (err) {
		XAL_DEBUG("FAILED: read_iab3_block(); err(%d)", err);
		return err;
	}

	switch (node->pos.level) {
	case 1:
		err = decode_iab3_node_records(xal, ag, block, index);
		if (err) {
			XAL_DEBUG("FAILED: decode_iab3_node(); err(%d)", err);
			return err;
		}
		break;

	case 0:
		err = decode_iab3_leaf_records(xal, ag, block, index);
		if (err) {
			XAL_DEBUG("FAILED: decode_iab3_leaf(); err(%d)", err);
			return err;
		}
		break;

	default:
		XAL_DEBUG("FAILED: iab3->level(%" PRIu16 ")?", node->pos.level);
		return -EINVAL;
	}

	XAL_DEBUG("EXIT");

	return 0;
}

int
xal_dinodes_retrieve(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint64_t index = 0;

	if (be->base.type != XAL_BACKEND_XFS) {
		XAL_DEBUG("SKIPPED: Backend is not XFS");
		return 0;
	}

	XAL_DEBUG("ENTER");

	be->dinodes_map = kh_init(ino_to_dinode);
	if (!be->dinodes_map) {
		XAL_DEBUG("FAILED: kh_init()");
		return -EINVAL;
	}

	be->dinodes = calloc(1, xal->sb.nallocated * xal->sb.inodesize);
	if (!be->dinodes) {
		XAL_DEBUG("FAILED: calloc()");
		return -errno;
	}

	for (uint32_t seqno = 0; seqno < xal->sb.agcount; ++seqno) {
		struct xal_ag *ag = &be->ags[seqno];
		int err;

		XAL_DEBUG("INFO: seqno: %" PRIu32 "", seqno);

		err = retrieve_dinodes_via_iab3(xal, ag, ag->agi_root, &index);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3(); err(%d)", err);
			free(be->dinodes);
			be->dinodes = NULL;
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Decodes the XFS Extent information in l0 and l1 into the given 'extent' structure
 *
 * @param l0 First 64bits; little-endian
 * @param l1 Last 64bits; little-endian
 */
static void
decode_xfs_extent(uint64_t l0, uint64_t l1, struct xal_extent *extent)
{
	// Extract start offset (l0:9-62)
	extent->start_offset = (l0 << 1) >> 10;

	// Extract start block (l0:0-8 and l1:21-63)
	extent->start_block = ((l0 & 0x1FF) << 43) | (l1 >> 21);

	// Extract block count (l1:0-20)
	extent->nblocks = l1 & 0x1FFFFF;

	extent->flag = l1 >> 63;
}

/**
 * Find the dinode with inode number 'ino'
 *
 * @return On success, 0 is returned. On error, -errno is returned to indicate the error.
 */
static int
dinodes_get(struct xal *xal, uint64_t ino, void **dinode)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	kh_ino_to_dinode_t *dinode_map = be->dinodes_map;
	khiter_t iter;

	iter = kh_get(ino_to_dinode, dinode_map, ino);
	if (iter == kh_end(dinode_map)) {
		XAL_DEBUG("FAILED: kh_get(0x%" PRIx64 ")", ino);
		return -EINVAL;
	}

	XAL_DEBUG("INFO: found ino(0x%" PRIx64 ")", ino);
	*dinode = kh_val(dinode_map, iter);

	return 0;
}

/**
 * Retrieve and decode the allocation group headers for a given allocation group
 *
 * This will retrieve the block containing the superblock and allocation-group headers. A subset of
 * the allocation group headers is decoded and xal->be->ags[seqo] is populated with the decoded data.
 *
 * Assumes the following:
 *
 * - The superblock (xal.sb) is initiatialized
 *
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param seqno The sequence number of the allocation group aka agno
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
static int
retrieve_and_decode_allocation_group(struct xnvme_dev *dev, void *buf, uint32_t seqno,
				     struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint8_t *cursor = buf;
	off_t offset = (off_t)seqno * (off_t)xal->sb.agblocks * (off_t)xal->sb.blocksize;
	struct xal_odf_agi *agi = (void *)(cursor + xal->sb.sectsize * 2);
	struct xal_odf_agf *agf = (void *)(cursor + xal->sb.sectsize);
	int err;

	err = dev_read(dev, buf, xal->sb.sectsize * 4, offset);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()");
		return err;
	}

	be->ags[seqno].seqno = seqno;
	be->ags[seqno].offset = offset;
	be->ags[seqno].agf_length = be32toh(agf->length);
	be->ags[seqno].agi_count = be32toh(agi->agi_count);
	be->ags[seqno].agi_level = be32toh(agi->agi_level);
	be->ags[seqno].agi_root = be32toh(agi->agi_root);

	/** minimalistic verification of headers **/
	assert(be32toh(agf->magicnum) == XAL_ODF_AGF_MAGIC);
	assert(be32toh(agi->magicnum) == XAL_ODF_AGI_MAGIC);
	assert(seqno == be32toh(agi->seqno));
	assert(seqno == be32toh(agf->seqno));

	return 0;
}

/**
 * Retrieve the superblock from disk and decode the on-disk-format and allocate 'xal' instance
 *
 * This will allocate the memory backing 'xal'
 *
 * @param dev Pointer to device instance
 * @param buf IO buffer, sufficiently large to hold a block of data
 * @param xal Double-pointer to the xal
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
static int
retrieve_and_decode_primary_superblock(struct xnvme_dev *dev, void *buf, struct xal **xal)
{
	const struct xal_odf_sb *psb = buf;
	struct xal *cand;
	struct xal_be_xfs *be;
	uint32_t agcount;
	int err;

	err = dev_read(dev, buf, 4096, 0);
	if (err) {
		XAL_DEBUG("FAILED: dev_read()\n");
		return -errno;
	}

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		XAL_DEBUG("FAILED: calloc()\n");
		return -errno;
	}

	cand->root_idx = XAL_POOL_IDX_NONE;
	cand->dirty = &cand->_dirty_storage;

	be = (struct xal_be_xfs *)&cand->be;

	agcount = be32toh(psb->agcount);

	be->ags = calloc(1, sizeof(*(be->ags)) * agcount);
	if (!be->ags) {
		XAL_DEBUG("FAILED: calloc()\n");
		return -errno;
	}

	// Setup the Superblock information subset; using big-endian conversion
	cand->sb.blocksize = be32toh(psb->blocksize);
	cand->sb.sectsize = be16toh(psb->sectsize);
	cand->sb.inodesize = be16toh(psb->inodesize);
	cand->sb.inopblock = be16toh(psb->inopblock);
	cand->sb.inopblog = psb->inopblog;
	cand->sb.icount = be64toh(psb->icount);
	cand->sb.rootino = be64toh(psb->rootino);
	cand->sb.agblocks = be32toh(psb->agblocks);
	cand->sb.agblklog = psb->agblklog;
	cand->sb.agcount = agcount;
	cand->sb.dirblocksize = cand->sb.blocksize << psb->dirblklog;

	*xal = cand;

	return 0;
}

int
xal_be_xfs_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts)
{
	struct xal *cand = NULL;
	struct xal_be_xfs *be;
	char shm_name[XAL_PATH_MAXLEN + 9];
	const char *shm;
	void *buf;
	int err;

	buf = xnvme_buf_alloc(dev, BUF_NBYTES);
	if (!buf) {
		XAL_DEBUG("FAILED: xnvme_buf_alloc()");
		return -errno;
	}

	err = retrieve_and_decode_primary_superblock(dev, buf, &cand);
	if (err) {
		XAL_DEBUG("FAILED: retrieve_and_decode_primary_superblock()");
		xnvme_buf_free(dev, buf);
		return -errno;
	}

	be = (struct xal_be_xfs *)&cand->be;

	be->base.type = XAL_BACKEND_XFS;
	be->base.close = xal_be_xfs_close;
	be->base.index = xal_be_xfs_index;

	be->buf = buf;

	for (uint32_t seqno = 0; seqno < cand->sb.agcount; ++seqno) {
		err = retrieve_and_decode_allocation_group(dev, buf, seqno, cand);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_and_decode_allocation_group(inodes); err(%d)",
				  err);
			goto failed;
		}

		cand->sb.nallocated += be->ags[seqno].agi_count;
	}

	if (opts->shm_name && strlen(opts->shm_name) > XAL_PATH_MAXLEN) {
		XAL_DEBUG("FAILED: shm_name too long");
		err = -EINVAL;
		goto failed;
	}

	shm = NULL;
	if (opts->shm_name) {
		snprintf(shm_name, sizeof(shm_name), "%s_inodes", opts->shm_name);
		shm = shm_name;
	}
	err = xal_pool_map(&cand->inodes, 40000000UL, cand->sb.nallocated, sizeof(struct xal_inode),
	                   shm);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(inodes); err(%d)", err);
		goto failed;
	}

	shm = NULL;
	if (opts->shm_name) {
		snprintf(shm_name, sizeof(shm_name), "%s_extents", opts->shm_name);
		shm = shm_name;
	}
	err = xal_pool_map(&cand->extents, 40000000UL, cand->sb.nallocated, sizeof(struct xal_extent),
	                   shm);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(extents); err(%d)", err);
		goto failed;
	}

	*xal = cand; // All is good; promote the candidate

	return 0;

failed:
	xal_close(cand);

	return err;
}

/**
 * Read the B+Tree block at 'fsbno' into 'buf'
 *
 * @param xal
 * @param fsbno File-System Block number in host-endianess
 * @param buf
 * @return On success 0 is returned. On error, negative error number is returned.
 */
static int
btree_lblock_read(struct xal *xal, uint64_t fsbno, void *buf)
{
	struct xal_odf_btree_lfmt *block = (void *)buf;
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	int err = -ENOSYS;

	XAL_DEBUG("ENTER: fsbno(0x%" PRIx64 ", %" PRIu64 ") @ ofz(%" PRIu64 ")", fsbno, fsbno, ofz);

	err = dev_read_into(xal->dev, be->buf, xal->sb.blocksize, ofz, buf);
	if (err) {
		XAL_DEBUG("FAILED: dev_read_into(); err(%d)", err);
		return err;
	}

	block->pos.level = be16toh(block->pos.level);
	block->pos.numrecs = be16toh(block->pos.numrecs);
	block->siblings.left = be64toh(block->siblings.left);
	block->siblings.right = be64toh(block->siblings.right);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", block->magic.text, block->magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", block->pos.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", block->pos.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%08" PRIx64 " @ %" PRIu64 ")", block->siblings.left,
		  xal_fsbno_offset(xal, block->siblings.left));
	XAL_DEBUG("INFO:    fsbno(0x%08" PRIx64 " @ %" PRIu64 ")", fsbno,
		  xal_fsbno_offset(xal, fsbno));
	XAL_DEBUG("INFO: rightsib(0x%08" PRIx64 " @ %" PRIu64 ")", block->siblings.left,
		  xal_fsbno_offset(xal, block->siblings.right));

	XAL_DEBUG("EXIT");

	return err;
}

/**
 * Decodes BMA3 Block of directory-extents in the given 'buf' and
 */
static int
btree_lblock_decode_leaf_records(struct xal *xal, void *buf, struct xal_inode *self)
{
	struct xal_odf_btree_lfmt *leaf = buf;
	struct pair_u64 *pairs = (void *)(((uint8_t *)buf) + sizeof(*leaf));
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;

	XAL_DEBUG("ENTER: Directory Extents -- B+Tree -- Leaf Node");

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(leaf->magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  leaf->magic.text, leaf->magic.num);
		return -EINVAL;
	}
	if (leaf->pos.level != 0) {
		XAL_DEBUG("FAILED: expecting a leaf; got level(%" PRIu16 ")", leaf->pos.level);
		return -EINVAL;
	}

	for (uint16_t rec = 0; rec < leaf->pos.numrecs; ++rec) {
		struct xal_extent extent = {0};

		XAL_DEBUG("rec(%" PRIu16 "), l0(0x%" PRIx64 "), l1(0x%" PRIx64 ")", rec,
			  pairs[rec].l0, pairs[rec].l1);

		decode_xfs_extent(be64toh(pairs[rec].l0), be64toh(pairs[rec].l1), &extent);

		for (size_t fsblk = 0; fsblk < extent.nblocks; fsblk += fsblk_per_dblk) {
			uint8_t dblock[ODF_BLOCK_FS_BYTES_MAX] = {0};
			uint64_t fsbno = extent.start_block + fsblk;

			union xal_odf_btree_magic *magic = (void *)(dblock);
			size_t ofz_disk = xal_fsbno_offset(xal, fsbno);
			int err;

			XAL_DEBUG("INFO:  fsbno(0x%" PRIu64 ") @ ofz(%" PRIu64 ")", fsbno,
				  xal_fsbno_offset(xal, fsbno));
			XAL_DEBUG("INFO:  fsblk(%zu : %zu/%zu)", fsblk, fsblk + 1, extent.nblocks);
			XAL_DEBUG("INFO:   dblk(%zu/%zu)", (fsblk / fsblk_per_dblk) + 1,
				  extent.nblocks / fsblk_per_dblk);

			err = dev_read_into(xal->dev, be->buf, xal->sb.dirblocksize, ofz_disk,
					    dblock);
			if (err) {
				XAL_DEBUG("FAILED: !dev_read(directory-extent)");
				return err;
			}

			XAL_DEBUG("INFO: magic('%.4s', 0x%" PRIx32 "); ", magic->text, magic->num);

			if ((be32toh(magic->num) != XAL_ODF_DIR3_DATA_MAGIC) &&
			    (be32toh(magic->num) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
				XAL_DEBUG("FAILED: looks like invalid magic value");
				return err;
			}

			for (uint64_t ofz = 64; ofz < xal->sb.dirblocksize;) {
				uint8_t *dentry_cursor = dblock + ofz;
				struct xal_inode dentry = {0};
				uint32_t slot;

				ofz += decode_dentry(dentry_cursor, &dentry);

				/**
				 * Seems like the only way to determine that there are no more
				 * entries are if one start to decode uinvalid entries.
				 * Such as a namelength of 0 or inode number 0.
				 * Thus, checking for that here.
				 */
				if ((!dentry.ino) || (!dentry.namelen)) {
					break;
				}

				/**
				 * Skip processing the mandatory dentries: '.' and '..'
				 */
				if ((dentry.namelen == 1) && (dentry.name[0] == '.')) {
					continue;
				}
				if ((dentry.namelen == 2) && (dentry.name[0] == '.') &&
				    (dentry.name[1] == '.')) {
					continue;
				}

				err = xal_pool_claim_inodes(&xal->inodes, 1, &slot);
				if (err) {
					XAL_DEBUG("FAILED: xal_pool_claim_inodes(...)");
					return err;
				}

				dentry.parent_idx = xal_inode_idx(xal, self);
				*xal_inode_at(xal, slot) = dentry;
				self->content.dentries.count += 1;
			}
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

static int
btree_lblock_decode_node_records(struct xal *XAL_UNUSED(xal), void *XAL_UNUSED(buf),
				 struct xal_inode *XAL_UNUSED(self))
{
	XAL_DEBUG("ENTER");

	printf("--==={[ I am so sorry about this ... ]}===--\n\n"
	       "Directory-Extents B+Tree with level > 0; currently not supported.\n"
	       "Triggering this state requires directories with 500K+ long-named files\n"
	       "Support will be added, however, less of a priority than other br0k3n things\n"
	       "--==={[ ... please send a PR :)      ]}===--\n");

	XAL_DEBUG("EXIT");

	return -ENOSYS;
}

/**
 * Retrieve a block decode it using the leaf and node helpers
 *
 * @param fsbno File-System Block number in host-endianess
 */
static int
btree_lblock_process(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	uint8_t block[ODF_BLOCK_FS_BYTES_MAX] = {0};
	struct xal_odf_btree_lfmt *lblock = (void *)block;
	int err;

	XAL_DEBUG("ENTER");

	err = btree_lblock_read(xal, fsbno, lblock);
	if (err) {
		XAL_DEBUG("FAILED: btree_lblock_read():err(%d)", err);
		return err;
	}

	switch (lblock->pos.level) {
	case 0:
		return btree_lblock_decode_leaf_records(xal, lblock, self);

	default:
		return btree_lblock_decode_node_records(xal, lblock, self);
	}

	XAL_DEBUG("EXIT");

	return err;
}

/**
 * B+tree Directories decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 20.5 B+tree Directories" for details
 */
static int
process_dinode_dir_btree_root(struct xal *xal, struct xal_odf_dinode *dinode,
			      struct xal_inode *self)
{
	void *dfork = ((uint8_t *)dinode) + sizeof(struct xal_odf_dinode);
	struct xal_odf_btree_pos pos = {0};
	uint64_t *fsbnos;
	size_t ofz_ptr; // Offset from start of dinode to start of embedded pointers
	int err;

	XAL_DEBUG("ENTER: Directory Extents -- B+Tree -- Root Node");

	pos.level = be16toh(*((uint16_t *)dfork));
	pos.numrecs = be16toh(*((uint16_t *)dfork));

	if (pos.level < 1) {
		XAL_DEBUG("FAILED: level(%" PRIu16 "); expected > 0", pos.level);
		return -EINVAL;
	}

	btree_dinode_meta(xal, dinode, NULL, NULL, &ofz_ptr);
	fsbnos = (void *)(((uint8_t *)dinode) + ofz_ptr);

	if (self->content.dentries.count) {
		XAL_DEBUG("INFO: dentries.count(%" PRIu32 ")", self->content.dentries.count);
		return -EINVAL;
	}
	self->content.dentries.inodes_idx = xal->inodes.free;

	XAL_DEBUG("=### Processing: File-System Block Pointers ###=");
	XAL_DEBUG("INFO: pos.numrecs(%" PRIu16 ")", pos.numrecs);
	for (uint16_t rec = 0; rec < pos.numrecs; ++rec) {
		XAL_DEBUG("INFO: ptr[%" PRIu16 "] = 0x%" PRIx64, rec, be64toh(fsbnos[rec]));

		err = btree_lblock_process(xal, be64toh(fsbnos[rec]), self);
		if (err) {
			XAL_DEBUG("FAILED: btree_lblock_process():err(%d)", err);
			return err;
		}
	}

	{
		/* Sort the entries in the directory to allow for binary search */
		struct xal_inode *all_inodes = (struct xal_inode *)xal->inodes.memory;
		struct xal_inode *these_inodes = &all_inodes[self->content.dentries.inodes_idx];
		uint32_t count = self->content.dentries.count;

		qsort(these_inodes, count, sizeof(struct xal_inode), compare_inode);
	}

	XAL_DEBUG("=### Processing: inodes constructed when chasing File-System Block Pointers")
	XAL_DEBUG("INFO: dentries.count(%" PRIu32 ")", self->content.dentries.count);
	for (uint32_t i = 0; i < self->content.dentries.count; ++i) {
		struct xal_inode *inode = xal_inode_at(xal, self->content.dentries.inodes_idx + i);

		XAL_DEBUG("INFO: inode->name(%.*s)", inode->namelen, inode->name);
		err = process_ino(xal, inode->ino, inode);
		if (err) {
			XAL_DEBUG("FAILED: process_ino():err(%d)", err);
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return err;
}

static int
process_file_btree_leaf(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	struct xal_odf_btree_lfmt leaf = {0};
	struct xal_extent *extents;
	uint32_t extent_start;
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Leaf Node");

	err = dev_read(xal->dev, be->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: dev_read(); err: %d", err);
		return err;
	}
	memcpy(&leaf, be->buf, sizeof(leaf));

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(leaf.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  leaf.magic.text, leaf.magic.num);
		return -EINVAL;
	}

	leaf.pos.level = be16toh(leaf.pos.level);
	if (leaf.pos.level != 0) {
		XAL_DEBUG("FAILED: expecting a leaf; got level(%" PRIu16 ")", leaf.pos.level);
		return -EINVAL;
	}
	leaf.pos.numrecs = be16toh(leaf.pos.numrecs);
	leaf.siblings.left = be64toh(leaf.siblings.left);
	leaf.siblings.right = be64toh(leaf.siblings.right);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", leaf.magic.text, leaf.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", leaf.pos.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", leaf.pos.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", leaf.siblings.left,
		  xal_fsbno_offset(xal, leaf.siblings.left));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", leaf.siblings.right,
		  xal_fsbno_offset(xal, leaf.siblings.right));

	err = xal_pool_claim_extents(&xal->extents, leaf.pos.numrecs, &extent_start);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_extents(); err(%d)", err);
		return err;
	}
	extents = xal_extent_at(xal, extent_start);
	self->content.extents.count += leaf.pos.numrecs;

	for (uint16_t rec = 0; rec < leaf.pos.numrecs; ++rec) {
		uint8_t *cursor = be->buf;
		uint64_t l0, l1;

		cursor += sizeof(leaf) + 16ULL * rec;

		l0 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		l1 = be64toh(*((uint64_t *)cursor));
		cursor += 8;

		decode_xfs_extent(l0, l1, &extents[rec]);
	}

	XAL_DEBUG("EXIT");

	return err;
}

static int
process_file_btree_node(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint64_t pointers[ODF_BLOCK_FS_BYTES_MAX / 8] = {0};
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	struct xal_odf_btree_lfmt node = {0};
	size_t pointers_ofz;
	size_t maxrecs;
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Internal Node");

	if (xal->sb.blocksize > ODF_BLOCK_FS_BYTES_MAX) {
		XAL_DEBUG("FAILED: blocksize(%" PRIu32 ") > ODF_BLOCK_FS_BYTES_MAX(%" PRIu64 ")",
			  xal->sb.blocksize, ODF_BLOCK_FS_BYTES_MAX);
		return -EINVAL;
	}

	btree_lblock_meta(xal, &maxrecs, NULL, &pointers_ofz);

	XAL_DEBUG("INFO: maxrecs(%zu)", maxrecs);
	XAL_DEBUG("INFO: pointers_ofz(%zu)", pointers_ofz);

	err = dev_read(xal->dev, be->buf, xal->sb.blocksize, ofz);
	if (err) {
		XAL_DEBUG("FAILED: dev_read(); err: %d", err);
		return err;
	}
	memcpy(&node, be->buf, sizeof(node));
	memcpy(&pointers, be->buf + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	if (XAL_ODF_BMAP_CRC_MAGIC != be32toh(node.magic.num)) {
		XAL_DEBUG("FAILED: expected magic(BMA3) got magic('%.4s', 0x%" PRIx32 "); ",
			  node.magic.text, node.magic.num);
		return -EINVAL;
	}

	node.pos.level = be16toh(node.pos.level);
	if (!node.pos.level) {
		XAL_DEBUG("FAILED: expecting a node; got level(%" PRIu16 ")", node.pos.level);
		return -EINVAL;
	}
	node.pos.numrecs = be16toh(node.pos.numrecs);
	node.siblings.left = be64toh(node.siblings.left);
	node.siblings.right = be64toh(node.siblings.right);

	XAL_DEBUG("INFO:    magic(%.4s, 0x%" PRIx32 ")", node.magic.text, node.magic.num);
	XAL_DEBUG("INFO:    level(%" PRIu16 ")", node.pos.level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", node.pos.numrecs);
	XAL_DEBUG("INFO:  leftsib(0x%016" PRIx64 " @ %" PRIu64 ")", node.siblings.left,
		  xal_fsbno_offset(xal, node.siblings.left));
	XAL_DEBUG("INFO:    fsbno(0x%016" PRIx64 " @ %" PRIu64 ")", fsbno, ofz);
	XAL_DEBUG("INFO: rightsib(0x%016" PRIx64 " @ %" PRIu64 ")", node.siblings.right,
		  xal_fsbno_offset(xal, node.siblings.right));

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < node.pos.numrecs; ++rec) {
		uint64_t pointer = be64toh(pointers[rec]);

		XAL_DEBUG("INFO:      ptr[%" PRIu16 "] = 0x%" PRIx64, rec, pointer);

		err = (node.pos.level == 1) ? process_file_btree_leaf(xal, pointer, self)
					    : process_file_btree_node(xal, pointer, self);
		if (err) {
			XAL_DEBUG("FAILED: file FMT_BTREE ino(0x%" PRIx64 ") @ ofz(%" PRIu64 ")",
				  self->ino, xal_ino_decode_absolute_offset(xal, self->ino));
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return err;
}

/**
 * B+tree Extent List decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 19.2 B+tree Extent List" for details
 *
 * Assumptions
 * ===========
 *
 * - Keys and pointers within the inode are 64 bits wide
 */
static int
process_dinode_file_btree_root(struct xal *xal, struct xal_odf_dinode *dinode,
			       struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint16_t level;	  // Level in the btree, expecting >= 1
	uint16_t numrecs; // Number of records in the inode itself
	size_t ofz_ptr;	  // Offset from start of dinode to start of embedded pointers
	int err;

	XAL_DEBUG("ENTER: File Extents -- B+Tree -- Root Node");

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	level = be16toh(*((uint16_t *)cursor));
	cursor += 2;

	numrecs = be16toh(*((uint16_t *)cursor));
	cursor += 2;

	if (level < 1) {
		XAL_DEBUG("FAILED: level(%" PRIu16 "); expected > 0", level);
		return -EINVAL;
	}

	XAL_DEBUG("INFO:    level(%" PRIu16 ")", level);
	XAL_DEBUG("INFO:  numrecs(%" PRIu16 ")", numrecs);

	btree_dinode_meta(xal, dinode, NULL, NULL, &ofz_ptr);

	// Let's try resetting the cursor...
	cursor = (uint8_t *)dinode + ofz_ptr;

	if (self->content.extents.count) {
		XAL_DEBUG("FAILED: self->content.extents.count(%" PRIu32 ")",
			  self->content.extents.count);
		return -EINVAL;
	}
	self->content.extents.extent_idx = xal->extents.free;

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < numrecs; ++rec) {
		uint64_t pointer = be64toh(*((uint64_t *)cursor));

		cursor += sizeof(pointer);

		XAL_DEBUG("INFO:      ptr[%" PRIu16 "] = 0x%" PRIx64, rec, pointer);

		err = (level == 1) ? process_file_btree_leaf(xal, pointer, self)
				   : process_file_btree_node(xal, pointer, self);
		if (err) {
			XAL_DEBUG("FAILED: file FMT_BTREE ino(0x%" PRIx64 " @ %" PRIu64 ")",
				  self->ino, xal_ino_decode_absolute_offset(xal, self->ino));
			return err;
		}
	}

	XAL_DEBUG("EXIT")

	return 0;
}

/**
 * Short Form Directories decoding and inode population
 *
 * @see XFS Algorithms & Data Structures - 3rd Edition - 20.1 Short Form Directories
 */
static int
process_dinode_dir_local(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	uint8_t *cursor = (void *)dinode;
	uint8_t count, i8count;
	int err;

	XAL_DEBUG("ENTER: Directory Entries -- Dinode Inline Shortform");

	cursor += sizeof(struct xal_odf_dinode); ///< Advance past inode data

	count = *cursor;
	cursor += 1; ///< Advance past count

	i8count = *cursor;
	cursor += 1; ///< Advance past i8count

	cursor += i8count ? 8 : 4; ///< Advance past parent inode number

	self->content.dentries.count = count;

	err = xal_pool_claim_inodes(&xal->inodes, count, &self->content.dentries.inodes_idx);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_inodes(); err(%d)", err);
		return err;
	}

	/** DECODE: namelen[1], offset[2], name[namelen], ftype[1], ino[4] | ino[8] */
	for (int i = 0; i < count; ++i) {
		struct xal_inode *dentry = xal_inode_at(xal, self->content.dentries.inodes_idx + i);

		dentry->namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(dentry->name, cursor, dentry->namelen);
		cursor += dentry->namelen; ///< Advance past 'name'

		dentry->ftype = *cursor;
		cursor += 1; ///< Advance past 'ftype'

		if (i8count) {
			i8count--;
			dentry->ino = be64toh(*(uint64_t *)cursor);
			cursor += 8; ///< Advance past 64-bit inode number
		} else {
			dentry->ino = be32toh(*(uint32_t *)cursor);
			cursor += 4; ///< Advance past 32-bit inode number
		}
	}

	{
		/* Sort the entries in the directory to allow for binary search */
		struct xal_inode *all_inodes = (struct xal_inode *)xal->inodes.memory;
		struct xal_inode *these_inodes = &all_inodes[self->content.dentries.inodes_idx];

		qsort(these_inodes, count, sizeof(struct xal_inode), compare_inode);
	}

	for (int i = 0; i < count; ++i) {
		struct xal_inode *dentry = xal_inode_at(xal, self->content.dentries.inodes_idx + i);

		dentry->parent_idx = xal_inode_idx(xal, self);
		err = process_ino(xal, dentry->ino, dentry);
		if (err) {
			XAL_DEBUG("FAILED: process_ino()");
			return err;
		}
	}

	XAL_DEBUG("EXIT");
	return 0;
}

/**
 * For some reason then di_big_nextents is populated. As far as i understand that should
 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
 * inode. Thus this abomination... just grabbing whatever has a value...
 */
static int
process_dinode_file_extents(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	struct pair_u64 *pairs = (void *)((uint8_t *)dinode + sizeof(*dinode));
	struct xal_extent *extents;
	uint64_t nextents;
	int err;

	nextents =
	    (dinode->di_nextents) ? be32toh(dinode->di_nextents) : be64toh(dinode->di_big_nextents);

	XAL_DEBUG("ENTER: File Extents -- Dinode Inline");
	XAL_DEBUG("INFO: name(%.*s)", self->namelen, self->name);
	XAL_DEBUG("INFO: nextents(%" PRIu64 ")", nextents);

	err = xal_pool_claim_extents(&xal->extents, nextents, &self->content.extents.extent_idx);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim()...");
		return err;
	}
	self->content.extents.count = nextents;

	extents = xal_extent_at(xal, self->content.extents.extent_idx);
	for (uint64_t rec = 0; rec < nextents; ++rec) {
		XAL_DEBUG("INFO: i(%" PRIu64 ")", rec);

		decode_xfs_extent(be64toh(pairs[rec].l0), be64toh(pairs[rec].l1), &extents[rec]);
	}

	XAL_DEBUG("INFO: content.dentries(%" PRIu32 ")", self->content.dentries.count);
	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Decode the dentry starting at the given buffer
 *
 * @return The size, in bytes and including alignment padding, of the decoded directory entry.
 */
static int
decode_dentry(void *buf, struct xal_inode *dentry)
{
	uint8_t *cursor = buf;
	uint16_t nbytes = 8 + 1 + 1 + 2;

	// xfs dir unused entries case with freetag value as 0xffff
	uint16_t freetag = be16toh(*(uint16_t *)cursor);
	if (freetag == 0xffff) {
		cursor += 2; // Advance length of uint16_t freetag
		uint16_t length = be16toh(*(uint16_t *)cursor);
		nbytes = length;
		return nbytes;
	}

	// xfs dir data entry case
	dentry->ino = be64toh(*(uint64_t *)cursor);
	cursor += 8;

	dentry->namelen = *cursor;
	cursor += 1;

	memcpy(dentry->name, cursor, dentry->namelen);
	cursor += dentry->namelen;

	dentry->ftype = *cursor;
	cursor += 1;

	// NOTE: Read 2byte tag is skipped
	// cursor += 2;

	nbytes += dentry->namelen;
	nbytes = ((nbytes + 7) / 8) * 8; ///< Ensure alignment to 8 byte boundary

	return nbytes;
}

/**
 * Read a directory-block from disk and process the directory-entries within.
 */
static int
process_dinode_dir_extents_dblock(struct xal *xal, uint64_t fsbno, struct xal_inode *self)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint8_t dblock[ODF_BLOCK_FS_BYTES_MAX] = {0};
	union xal_odf_btree_magic *magic = (void *)(dblock);
	size_t ofz_disk = xal_fsbno_offset(xal, fsbno);
	int err;

	XAL_DEBUG("ENTER");

	err = dev_read_into(xal->dev, be->buf, xal->sb.dirblocksize, ofz_disk, dblock);
	if (err) {
		XAL_DEBUG("FAILED: !dev_read(directory-extent)");
		return err;
	}

	XAL_DEBUG("INFO: magic('%.4s', 0x%" PRIx32 "); ", magic->text, magic->num);

	if ((be32toh(magic->num) != XAL_ODF_DIR3_DATA_MAGIC) &&
	    (be32toh(magic->num) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
		XAL_DEBUG("FAILED: looks like invalid magic value");
		return err;
	}

	for (uint64_t ofz = 64; ofz < xal->sb.dirblocksize;) {
		uint8_t *dentry_cursor = dblock + ofz;
		struct xal_inode dentry = {0};
		uint32_t slot;

		ofz += decode_dentry(dentry_cursor, &dentry);

		/**
		 * Seems like the only way to determine that there are no more
		 * entries are if one start to decode uinvalid entries.
		 * Such as a namelength of 0 or inode number 0.
		 * Thus, checking for that here.
		 */
		if ((!dentry.ino) || (!dentry.namelen)) {
			break;
		}

		/**
		 * Skip processing the mandatory dentries: '.' and '..'
		 */
		if ((dentry.namelen == 1) && (dentry.name[0] == '.')) {
			continue;
		}
		if ((dentry.namelen == 2) && (dentry.name[0] == '.') && (dentry.name[1] == '.')) {
			continue;
		}

		err = xal_pool_claim_inodes(&xal->inodes, 1, &slot);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_claim_inodes(...)");
			return err;
		}

		dentry.parent_idx = xal_inode_idx(xal, self);
		*xal_inode_at(xal, slot) = dentry;
		self->content.dentries.count += 1;
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Processing a multi-block directory with extents in inline format
 * ================================================================
 *
 * - Extract and decode the extents embedded within the dinode
 *
 *   - Unlike data-extents, then these directory-extents will not be stored the tree
 *
 * - Retrieve the blocks, from disk, described by the extents
 *
 * - Decode the directory entry-descriptions into 'xal_inode'
 *
 *   - Initially setting up 'self.content.dentries.inodes'
 *   - Incrementing 'self.content.dentries.count'
 *   - WARNING: When this is running, then no one else should be claiming memory from the pool
 *
 * An upper bound on extents
 * -------------------------
 *
 * There is an upper-bound of how many extents there can be in this case, that is, the amount that
 * can reside inside the inode. An approximation to this amount is:
 *
 *   nextents_max = (xal.sb.inodesize - header) / 16
 *
 * Thus, with an inode-size of 512 and the dinode size of about 176, then there is room for at most
 * 21 extents.
 *
 * An upper bound on directory entries
 * -----------------------------------
 *
 * An extent can describe a very large range of blocks, thus, it seems like there is not a trivial
 * way to put a useful upper-bound on it. E.g. even with a very small amount of extents, then each
 * of these have a 'count' of blocks. This 200bits worth of blocks... thats an awful lot of blocks.
 */

static int
process_dinode_dir_extents(struct xal *xal, struct xal_odf_dinode *dinode, struct xal_inode *self)
{
	struct pair_u64 *extents = (void *)(((uint8_t *)dinode) + sizeof(struct xal_odf_dinode));
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	uint64_t nextents = be32toh(dinode->di_nextents);
	int64_t nbytes = be64toh(dinode->size);
	int err;

	/**
	 * For some reason then di_big_nextents is populated. As far as i understand that should
	 * not happen for format=0x2 "extents" as this should have all extent-records inline in the
	 * inode. Thus this abomination... just grabbing whatever has a value...
	 */
	if (!nextents) {
		nextents = be64toh(dinode->di_big_nextents);
	}
	XAL_DEBUG("INFO:       nextents(%" PRIu64 ")", nextents);
	XAL_DEBUG("INFO: fsblk_per_dblk(%" PRIu32 ")", fsblk_per_dblk);
	XAL_DEBUG("INFO:         nbytes(%" PRIu64 ")", nbytes);

	self->content.dentries.inodes_idx = xal->inodes.free;

	/**
	 * Decode the extents and process each block
	 */
	for (uint64_t i = 0; (nbytes > 0) && (i < nextents); ++i) {
		struct xal_extent extent = {0};

		XAL_DEBUG("INFO: extent(%" PRIu64 "/%" PRIu64 ")", i + 1, nextents);
		XAL_DEBUG("INFO: nbytes(%" PRIu64 ")", nbytes);

		decode_xfs_extent(be64toh(extents[i].l0), be64toh(extents[i].l1), &extent);

		for (size_t fsblk = 0; fsblk < extent.nblocks; fsblk += fsblk_per_dblk) {
			uint64_t fsbno = extent.start_block + fsblk;

			XAL_DEBUG("INFO:  fsbno(0x%" PRIu64 ") @ ofz(%" PRIu64 ")", fsbno,
				  xal_fsbno_offset(xal, fsbno));
			XAL_DEBUG("INFO:  fsblk(%zu : %zu/%zu)", fsblk, fsblk + 1, extent.nblocks);
			XAL_DEBUG("INFO:   dblk(%zu/%zu)", (fsblk / fsblk_per_dblk) + 1,
				  extent.nblocks / fsblk_per_dblk);

			err = process_dinode_dir_extents_dblock(xal, fsbno, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_extents_block():err(%d)",
					  err);
				return err;
			}
		}

		nbytes -= extent.nblocks * xal->sb.blocksize;
	}

	{
		/* Sort the entries in the directory to allow for binary search */
		struct xal_inode *all_inodes = (struct xal_inode *)xal->inodes.memory;
		struct xal_inode *these_inodes = &all_inodes[self->content.dentries.inodes_idx];
		uint32_t count = self->content.dentries.count;

		qsort(these_inodes, count, sizeof(struct xal_inode), compare_inode);
	}

	XAL_DEBUG("=### Processing: inodes constructed when decoding dir(FMT_EXTENTS)")
	for (uint32_t i = 0; i < self->content.dentries.count; ++i) {
		struct xal_inode *inode = xal_inode_at(xal, self->content.dentries.inodes_idx + i);

		err = process_ino(xal, inode->ino, inode);
		if (err) {
			XAL_DEBUG("FAILED: process_ino():err(%d)", err);
			return err;
		}
	}

	return err;
}

/**
 * Internal helper recursively traversing the on-disk-format to build an index of the file-system
 */
static int
process_ino(struct xal *xal, uint64_t ino, struct xal_inode *self)
{
	struct xal_odf_dinode *dinode;
	int err;

	XAL_DEBUG("ENTER");

	err = dinodes_get(xal, ino, (void **)&dinode);
	if (err) {
		XAL_DEBUG("FAILED: dinodes_get(); err(%d)", err);
		return err;
	}

	if (!self->ftype) {
		uint16_t mode = be16toh(dinode->di_mode);

		if (S_ISDIR(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_DIR;
		} else if (S_ISREG(mode)) {
			self->ftype = XAL_ODF_DIR3_FT_REG_FILE;
		} else {
			XAL_DEBUG("FAILED: unsupported ftype");
			return -EINVAL;
		}
	}

	self->size = be64toh(dinode->size);
	self->ino = be64toh(dinode->ino);

	XAL_DEBUG("INFO: ino(0x%" PRIx64 ") @ ofz(%" PRIu64 "), name(%.*s)[%" PRIu8 "]", ino,
		  xal_ino_decode_absolute_offset(xal, ino), self->namelen, self->name,
		  self->namelen);
	XAL_DEBUG("INFO: format(0x%" PRIu8 ")", dinode->di_format);

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_BTREE:
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			self->content.dentries.inodes_idx = xal->inodes.free;

			err = process_dinode_dir_btree_root(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_btree():err(%d)", err);
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_dinode_file_btree_root(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_file_btree_root():err(%d)", err);
				return err;
			}
			break;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in BTREE fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_EXTENTS:
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_dir_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_extents()");
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			err = process_dinode_file_extents(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_file_extents()");
				return err;
			}
			break;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in EXTENTS fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_LOCAL: ///< Decode directory listing in inode
		switch (self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = process_dinode_dir_local(xal, dinode, self);
			if (err) {
				XAL_DEBUG("FAILED: process_dinode_dir_local()");
				return err;
			}
			break;

		case XAL_ODF_DIR3_FT_REG_FILE:
			XAL_DEBUG("FAILED: file in LOCAL fmt -- not implemented.");
			return -ENOSYS;

		default:
			XAL_DEBUG("FAILED: Unsupported file-type in BTREE fmt");
			return -ENOSYS;
		}
		break;

	case XAL_DINODE_FMT_DEV:
	case XAL_DINODE_FMT_UUID:
		XAL_DEBUG("FAILED: Unsupported FMT_DEV or FMT_UUID");
		return -ENOSYS;
	}

	XAL_DEBUG("EXIT");

	return 0;
}

int
xal_be_xfs_index(struct xal *xal)
{
	struct xal_inode *root;
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	int err;

	if (!be->dinodes) {
		return -EINVAL;
	}

	xal_pool_clear(&xal->inodes);
	xal_pool_clear(&xal->extents);

	err = xal_pool_claim_inodes(&xal->inodes, 1, &xal->root_idx);
	if (err) {
		return err;
	}

	root = xal_inode_at(xal, xal->root_idx);
	root->ino = xal->sb.rootino;
	root->ftype = XAL_ODF_DIR3_FT_DIR;
	root->namelen = 0;
	root->parent_idx = XAL_POOL_IDX_NONE;
	root->content.extents.count = 0;
	root->content.dentries.count = 0;

	err = process_ino(xal, root->ino, root);
	if (err) {
		XAL_DEBUG("FAILED: process_ino(); err(%d)", err);
		return err;
	}

	atomic_store(xal->dirty, false);

	return err;
}
