#define _GNU_SOURCE /* ahead of every header, for mkostemp() */
#include <asm-generic/errno.h>
#include <libxnvme.h>

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <limits.h>
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

/* An inode chunk from the inobt, read once all of them are known */
struct chunk {
	uint64_t ofz;	    ///< Byte offset of the chunk on the device
	uint64_t startino;  ///< Number of its first inode
	uint64_t allocated; ///< Bit per inode, set when it is allocated: not free, not in a hole
	size_t base;	    ///< Index in be->dinodes of its first allocated inode
};

/**
 * How an inode is found in be->dinodes, which be->dinodes_map points at
 *
 * After the inobt walk the inodes are in be->dinodes in the order of their numbers, as the
 * chunks list them, so an inode is found by its chunk, and its rank among the allocated inodes
 * there. A subtree is read without the inobt, in no such order, and its inodes go in a map.
 */
struct dinodes_index {
	struct chunk *chunks; ///< Sorted by startino; NULL with a subtree
	size_t nchunks;
	khash_t(ino_to_dinode) * map; ///< NULL without a subtree
	size_t ndinodes;	      ///< Inodes in be->dinodes
};

/* Free be->dinodes and what finds an inode there */
static void
dinodes_free(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dinodes_index *index = be->dinodes_map;

	if (index) {
		if (index->map) {
			kh_destroy(ino_to_dinode, index->map);
		}
		free(index->chunks);
		free(index);
		be->dinodes_map = NULL;
	}
	free(be->dinodes);
	be->dinodes = NULL;
}

/* A block read ahead by dirblocks_read(): its bytes and how many of them were read */
struct dirblock {
	uint8_t *data;
	uint32_t nbytes;
};
KHASH_MAP_INIT_INT64(ofz_to_block, struct dirblock);

static int
decode_dentry(void *buf, struct xal_inode *dentry);

struct chunks;

static int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno,
			  struct chunks *chunks);

static int
process_ino(struct xal *xal, uint64_t ino, struct xal_inode *self);

int
xal_be_xfs_index(struct xal *xal);

static void
dirblocks_free(struct xal *xal);

static int
dinode_read(struct xal *xal, uint64_t ino, struct xal_odf_dinode **dinode);

static int
subtree_read(struct xal *xal);

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

/* Reads in flight at once when batch_read() has an xNVMe queue */
#define BATCH_QDEPTH 32

/* A hole of up to this many bytes between two items is read through rather than split at */
#define BATCH_GAP_NBYTES (64 * 1024UL)

struct batch_item {
	uint64_t ofz;	 ///< Byte offset on the device
	uint32_t nbytes; ///< Number of bytes to read
	uint32_t idx;	 ///< The caller's index for the item
};

/**
 * Called by batch_read() once for each item with its bytes, in no particular order. A non-zero
 * return stops further reads and is what batch_read() returns.
 */
typedef int (*batch_item_fn)(struct xal *xal, const struct batch_item *item, const uint8_t *data,
			     void *arg);

struct batch;

struct batch_slot {
	struct batch *batch;
	uint8_t *buf;
	uint64_t ofz;  ///< Device offset of the read in this slot
	size_t first;  ///< Index of the first item the read covers
	size_t nitems; ///< Number of items the read covers
	bool busy;
};

struct batch {
	struct xal *xal;
	struct batch_item *items;
	batch_item_fn fn;
	void *arg;
	int err;
	uint32_t nslots;
	struct batch_slot slots[BATCH_QDEPTH];
};

static int
batch_item_cmp(const void *a, const void *b)
{
	const struct batch_item *x = a, *y = b;

	return (x->ofz > y->ofz) - (x->ofz < y->ofz);
}

static void
batch_deliver(struct batch_slot *slot)
{
	struct batch *batch = slot->batch;

	for (size_t i = slot->first; i < slot->first + slot->nitems && !batch->err; ++i) {
		const struct batch_item *item = &batch->items[i];

		batch->err =
		    batch->fn(batch->xal, item, slot->buf + (item->ofz - slot->ofz), batch->arg);
	}
	slot->busy = false;
}

static void
batch_complete(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct batch_slot *slot = cb_arg;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		XAL_DEBUG("FAILED: read at ofz(%" PRIu64 ")", slot->ofz);
		if (!slot->batch->err) {
			slot->batch->err = -EIO;
		}
		slot->busy = false;
	} else {
		batch_deliver(slot);
	}

	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

/**
 * Read the given items and hand each one's bytes to 'fn'
 *
 * The items are sorted by offset (in place) and merged into reads of at most BUF_NBYTES, or the
 * device's MDTS rounded down to whole filesystem blocks when smaller, that also read through holes
 * of up to BATCH_GAP_NBYTES. They are submitted through an xNVMe queue of capacity BATCH_QDEPTH on
 * the device; how many are in flight at once is up to the device's async interface (xNVMe's
 * default, emu, runs them one at a time). The buffers come from one xnvme_buf_alloc() of
 * BATCH_QDEPTH reads, halved until it succeeds, or are be->buf when none can be had or the device
 * gives no queue; reads then go one at a time. An item that is empty, not aligned to the LBA size,
 * or larger than a read is rejected with -EINVAL.
 *
 * @return On success, 0 is returned. On error, negative errno is returned.
 */
static int
batch_read(struct xal *xal, struct batch_item *items, size_t nitems, batch_item_fn fn, void *arg)
{
	const struct xnvme_geo *geo = xnvme_dev_get_geo(xal->dev);
	uint64_t max_nbytes = BUF_NBYTES < geo->mdts_nbytes ? BUF_NBYTES : geo->mdts_nbytes;
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct batch batch = {.xal = xal, .items = items, .fn = fn, .arg = arg};
	struct xnvme_queue *queue = NULL;
	uint8_t *mem = NULL;
	int err;

	/* xNVMe caps the MDTS for Linux NVMe ioctls at a count of LBAs (0.7.5: 127; main:
	 * max_segments - 1 physical blocks); keep the read buffers whole filesystem blocks */
	if (max_nbytes >= xal->sb.blocksize) {
		max_nbytes -= max_nbytes % xal->sb.blocksize;
	}
	if (nitems > UINT32_MAX) {
		return -EINVAL;
	}
	for (size_t i = 0; i < nitems; ++i) {
		if (!items[i].nbytes || items[i].nbytes > max_nbytes ||
		    items[i].ofz % geo->lba_nbytes || items[i].nbytes % geo->lba_nbytes) {
			XAL_DEBUG("FAILED: item ofz(%" PRIu64 ") nbytes(%" PRIu32 ")", items[i].ofz,
				  items[i].nbytes);
			return -EINVAL;
		}
	}
	if (!nitems) {
		return 0;
	}

	qsort(items, nitems, sizeof(*items), batch_item_cmp);

	for (batch.nslots = BATCH_QDEPTH; batch.nslots > 1 && !mem; batch.nslots /= 2) {
		mem = xnvme_buf_alloc(xal->dev, batch.nslots * max_nbytes);
		if (mem && xnvme_queue_init(xal->dev, batch.nslots, 0, &queue)) {
			XAL_DEBUG("INFO: no queue, reading one at a time");
			xnvme_buf_free(xal->dev, mem);
			mem = NULL;
			queue = NULL;
			break;
		}
		if (mem) {
			break;
		}
	}
	if (!mem) {
		batch.nslots = 1;
	}
	for (uint32_t s = 0; s < batch.nslots; ++s) {
		batch.slots[s].batch = &batch;
		batch.slots[s].buf = mem ? mem + s * max_nbytes : be->buf;
	}

	for (size_t i = 0; i < nitems && !batch.err;) {
		uint64_t ofz = items[i].ofz, end = ofz + items[i].nbytes;
		struct batch_slot *slot = NULL;
		struct xnvme_cmd_ctx *ctx;
		size_t first = i;

		for (++i; i < nitems; ++i) {
			uint64_t item_end = items[i].ofz + items[i].nbytes;
			uint64_t new_end = item_end > end ? item_end : end;

			if (items[i].ofz > end + BATCH_GAP_NBYTES || new_end - ofz > max_nbytes) {
				break;
			}
			end = new_end;
		}

		while (!slot && !batch.err) {
			for (uint32_t s = 0; s < batch.nslots && !slot; ++s) {
				if (!batch.slots[s].busy) {
					slot = &batch.slots[s];
				}
			}
			if (!slot) {
				err = xnvme_queue_poke(queue, 0);
				if (err < 0) {
					XAL_DEBUG("FAILED: xnvme_queue_poke(); err(%d)", err);
					batch.err = err;
				}
			}
		}
		if (batch.err) {
			break;
		}
		slot->ofz = ofz;
		slot->first = first;
		slot->nitems = i - first;
		slot->busy = true;

		if (!queue) {
			err = dev_read(xal->dev, slot->buf, end - ofz, ofz);
			if (err) {
				batch.err = err;
				break;
			}
			batch_deliver(slot);
			continue;
		}

		/* A free slot means a free command context: the queue has one per slot */
		ctx = xnvme_queue_get_cmd_ctx(queue);
		if (!ctx) {
			XAL_DEBUG("FAILED: xnvme_queue_get_cmd_ctx()");
			slot->busy = false;
			batch.err = -ENOMEM;
			break;
		}
		xnvme_cmd_ctx_set_cb(ctx, batch_complete, slot);
		err = xnvme_nvm_read(ctx, xnvme_dev_get_nsid(xal->dev), ofz / geo->lba_nbytes,
				     (end - ofz) / geo->lba_nbytes - 1, slot->buf, NULL);
		if (err) {
			XAL_DEBUG("FAILED: xnvme_nvm_read(); err(%d)", err);
			xnvme_queue_put_cmd_ctx(queue, ctx);
			slot->busy = false;
			batch.err = err;
		}
	}

	if (queue) {
		err = xnvme_queue_drain(queue);
		if (err < 0 && !batch.err) {
			batch.err = err;
		}
		xnvme_queue_term(queue);
	}
	if (mem) {
		xnvme_buf_free(xal->dev, mem);
	}

	return batch.err;
}

/* Directory and bmbt blocks read ahead for one xal_index() */
struct dirblocks {
	kh_ofz_to_block_t *map; ///< Device offset to the bytes of the block
	uint8_t **areas;	///< The blocks of each read-ahead round
	size_t nareas;
};

/**
 * Read a block the index needs into 'buf': from the blocks dirblocks_read() read ahead when it is
 * one of them, from the device otherwise
 */
static int
read_index_block(struct xal *xal, uint64_t ofz, size_t nbytes, void *buf)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks = be->dirblocks;

	if (dirblocks) {
		khiter_t iter = kh_get(ofz_to_block, dirblocks->map, ofz);

		if (iter != kh_end(dirblocks->map) &&
		    nbytes <= kh_value(dirblocks->map, iter).nbytes) {
			memcpy(buf, kh_value(dirblocks->map, iter).data, nbytes);
			return 0;
		}
	}

	return dev_read_into(xal->dev, be->buf, nbytes, ofz, buf);
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

	be = (struct xal_be_xfs *)&xal->be;

	if (xal->dev && be->buf) {
		xnvme_buf_free(xal->dev, be->buf);
	}

	dinodes_free(xal);
	dirblocks_free(xal);
	if (be->tree) {
		free(be->tree->subtree);
		free(be->tree);
	}
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

/* The chunks the inobt walk gathers, in the order of their inode numbers */
struct chunks {
	struct chunk *chunks;
	size_t nchunks;
	size_t capacity;
	uint64_t ninodes; ///< Allocated inodes in the chunks gathered so far
};

static int
decode_iab3_leaf_records(struct xal *xal, struct xal_ag *ag, void *buf, struct chunks *chunks)
{
	struct xal_odf_btree_sfmt *root = (void *)buf;

	XAL_DEBUG("ENTER");

	if (root->pos.numrecs >
	    (xal->sb.blocksize - sizeof(*root)) / sizeof(struct xal_odf_inobt_rec)) {
		XAL_DEBUG("FAILED: inobt leaf numrecs(%" PRIu16 ")", root->pos.numrecs);
		return -EUCLEAN;
	}

	for (uint16_t reci = 0; reci < root->pos.numrecs; ++reci) {
		struct xal_odf_inobt_rec *rec;
		uint64_t startino, allocated, holes;
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

		/* A chunk starts on a multiple of its inodes; one that does not start a block
		 * (more than CHUNK_NINO inodes in a block) is not handled */
		if (rec->startino % CHUNK_NINO) {
			XAL_DEBUG("FAILED: inobt startino(0x%" PRIx32 ")", rec->startino);
			return -EUCLEAN;
		}
		if (agbino) {
			return -EOPNOTSUPP;
		}

		if (chunks->nchunks == chunks->capacity) {
			size_t capacity = chunks->capacity ? chunks->capacity * 2 : 1024;
			struct chunk *grown = realloc(chunks->chunks, capacity * sizeof(*grown));

			if (!grown) {
				XAL_DEBUG("FAILED: realloc()");
				return -ENOMEM;
			}
			chunks->chunks = grown;
			chunks->capacity = capacity;
		}
		startino =
		    ((uint64_t)ag->seqno << (xal->sb.agblklog + xal->sb.inopblog)) | rec->startino;
		if (chunks->nchunks && startino <= chunks->chunks[chunks->nchunks - 1].startino) {
			XAL_DEBUG("FAILED: inobt record startino(0x%" PRIx64 ") out of order",
				  startino);
			return -EUCLEAN;
		}
		/* The holes of a sparse chunk are free too, as xchk_iallocbt_rec() checks */
		holes = 0;
		for (int bit = 0; bit < 16; ++bit) {
			if ((rec->holemask >> bit) & 1) {
				holes |= 0xfULL << (bit * 4);
			}
		}
		if ((rec->free & holes) != holes) {
			XAL_DEBUG("FAILED: inobt record startino(0x%" PRIx64 ") holes not free",
				  startino);
			return -EUCLEAN;
		}
		allocated = ~rec->free;
		chunks->chunks[chunks->nchunks++] = (struct chunk){
		    .ofz = agbno * xal->sb.blocksize + ag->offset,
		    .startino = startino,
		    .allocated = allocated,
		    .base = chunks->ninodes,
		};
		chunks->ninodes += __builtin_popcountll(allocated);
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Copy the allocated inodes of the chunk read for 'item' to its place in be->dinodes; a
 * batch_item_fn
 */
static int
decode_chunk(struct xal *xal, const struct batch_item *item, const uint8_t *data, void *arg)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct chunks *chunks = arg;
	const struct chunk *chunk = &chunks->chunks[item->idx];
	uint8_t *dinode = &be->dinodes[chunk->base * xal->sb.inodesize];

	for (uint8_t chunk_index = 0; chunk_index < CHUNK_NINO; ++chunk_index) {
		if (!((chunk->allocated >> chunk_index) & 1)) {
			continue;
		}
		memcpy(dinode, &data[chunk_index * xal->sb.inodesize], xal->sb.inodesize);
		dinode += xal->sb.inodesize;
	}

	return 0;
}

/**
 * Decodes the node and invokes retrieve_dinodes_via_iab3() for each decoded record.
 */
static int
decode_iab3_node_records(struct xal *xal, struct xal_ag *ag, void *buf, struct chunks *chunks)
{
	uint32_t pointers[ODF_BLOCK_FS_BYTES_MAX / sizeof(uint32_t)] = {0};
	struct xal_odf_btree_sfmt *node = (void *)buf;
	uint8_t *cursor = buf;
	size_t pointers_ofz, maxrecs;
	int err;

	XAL_DEBUG("ENTER");

	btree_sblock_meta(xal, &maxrecs, NULL, &pointers_ofz);
	if (node->pos.numrecs > maxrecs) {
		XAL_DEBUG("FAILED: inobt node numrecs(%" PRIu16 ")", node->pos.numrecs);
		return -EUCLEAN;
	}

	memcpy(&pointers, cursor + pointers_ofz, xal->sb.blocksize - pointers_ofz);

	XAL_DEBUG("#### Processing Pointers ###");
	for (uint16_t rec = 0; rec < node->pos.numrecs; ++rec) {
		uint32_t blkno = be32toh(pointers[rec]);

		XAL_DEBUG("INFO: ptr[%" PRIu16 "] = 0x%" PRIx32, rec, blkno);
		err = retrieve_dinodes_via_iab3(xal, ag, blkno, chunks);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3() : err(%d)", err);
			return err;
		}
	}

	XAL_DEBUG("EXIT");

	return 0;
}

/**
 * Gather the inode chunks recorded in the inode-allocation-b+tree of the given allocation group
 *
 * It is assumed that the inode-allocation-b+tree is rooted at the given 'blkno'
 */
static int
retrieve_dinodes_via_iab3(struct xal *xal, struct xal_ag *ag, uint64_t blkno, struct chunks *chunks)
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
		err = decode_iab3_node_records(xal, ag, block, chunks);
		if (err) {
			XAL_DEBUG("FAILED: decode_iab3_node(); err(%d)", err);
			return err;
		}
		break;

	case 0:
		err = decode_iab3_leaf_records(xal, ag, block, chunks);
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

static int
dinodes_retrieve(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dinodes_index *index;
	struct chunks chunks = {0};
	struct batch_item *items = NULL;
	int err = 0;

	XAL_DEBUG("ENTER");

	dinodes_free(xal);

	index = calloc(1, sizeof(*index));
	if (!index) {
		XAL_DEBUG("FAILED: calloc()");
		return -ENOMEM;
	}
	be->dinodes_map = index;

	be->dinodes = calloc(1, xal->sb.nallocated * xal->sb.inodesize);
	if (!be->dinodes) {
		XAL_DEBUG("FAILED: calloc()");
		err = -ENOMEM;
		goto exit;
	}

	if (be->tree->subtree) {
		index->map = kh_init(ino_to_dinode);
		if (!index->map) {
			XAL_DEBUG("FAILED: kh_init()");
			err = -ENOMEM;
			goto exit;
		}
		err = subtree_read(xal);
		goto exit;
	}

	/* Walk every inobt first, so that all the chunks are known and can be read together */
	for (uint32_t seqno = 0; seqno < xal->sb.agcount && !err; ++seqno) {
		struct xal_ag *ag = &be->ags[seqno];

		XAL_DEBUG("INFO: seqno: %" PRIu32 "", seqno);

		err = retrieve_dinodes_via_iab3(xal, ag, ag->agi_root, &chunks);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_dinodes_via_iab3(); err(%d)", err);
		}
	}

	if (err) {
		goto exit;
	}
	if (chunks.ninodes > xal->sb.nallocated) {
		XAL_DEBUG("FAILED: more allocated inodes than the AGIs count(%" PRIu64 ")",
			  xal->sb.nallocated);
		err = -EUCLEAN;
		goto exit;
	}

	items = calloc(chunks.nchunks ? chunks.nchunks : 1, sizeof(*items));
	if (!items) {
		XAL_DEBUG("FAILED: calloc()");
		err = -ENOMEM;
		goto exit;
	}
	for (size_t i = 0; i < chunks.nchunks; ++i) {
		items[i] = (struct batch_item){
		    .ofz = chunks.chunks[i].ofz,
		    .nbytes = (CHUNK_NINO / xal->sb.inopblock) * xal->sb.blocksize,
		    .idx = i,
		};
	}
	err = batch_read(xal, items, chunks.nchunks, decode_chunk, &chunks);
	if (err) {
		XAL_DEBUG("FAILED: batch_read(chunks); err(%d)", err);
		goto exit;
	}
	index->chunks = chunks.chunks;
	index->nchunks = chunks.nchunks;
	index->ndinodes = chunks.ninodes;
	chunks.chunks = NULL;

exit:
	free(items);
	free(chunks.chunks);

	if (err) {
		dirblocks_free(xal);
		dinodes_free(xal);
		return err;
	}

	XAL_DEBUG("EXIT");

	return 0;
}

int
xal_dinodes_retrieve(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	int err;

	if (xal->procrole == XAL_PROCROLE_SECONDARY) {
		return -EINVAL;
	}

	if (be->base.type != XAL_BACKEND_XFS) {
		XAL_DEBUG("SKIPPED: Backend is not XFS");
		return 0;
	}

	/* Not while xal_index(), xal_index_save() or xal_index_load() uses the inodes or be->buf */
	if (atomic_exchange(&xal->indexing, true)) {
		return -EBUSY;
	}
	err = dinodes_retrieve(xal);
	atomic_store(&xal->indexing, false);

	return err;
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
	struct dinodes_index *index = be->dinodes_map;
	struct xal_odf_dinode *found;
	const struct chunk *chunk;
	size_t lo = 0, hi = index->nchunks;
	uint64_t offset;

	if (index->map) {
		khiter_t iter = kh_get(ino_to_dinode, index->map, ino);

		/* Not read ahead by subtree_read(); without an inobt walk that is no error */
		if (iter == kh_end(index->map)) {
			return dinode_read(xal, ino, (struct xal_odf_dinode **)dinode);
		}
		*dinode = kh_val(index->map, iter);
		return 0;
	}

	/* The last chunk starting at or before 'ino' */
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (index->chunks[mid].startino <= ino) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	chunk = lo ? &index->chunks[lo - 1] : NULL;
	offset = chunk ? ino - chunk->startino : CHUNK_NINO;
	if (offset >= CHUNK_NINO || !((chunk->allocated >> offset) & 1)) {
		XAL_DEBUG("FAILED: no allocated inode(0x%" PRIx64 ")", ino);
		return -EINVAL;
	}

	found = (void *)&be->dinodes[(chunk->base + __builtin_popcountll(chunk->allocated &
									 ((1ULL << offset) - 1))) *
				     xal->sb.inodesize];
	/* The rank is only as good as the inobt record; the inode says which it is */
	if (be16toh(found->di_magic) != XAL_DINODE_MAGIC || be64toh(found->ino) != ino) {
		XAL_DEBUG("FAILED: the inode in the place of ino(0x%" PRIx64 ") is another", ino);
		return -EUCLEAN;
	}
	*dinode = found;

	return 0;
}

/*
 * Symlinks and special files are not indexed; an unknown type is read from the inode's mode.
 * Returns 1 to skip the entry, 0 to index it, or a negative errno, -EINVAL for a type XFS does
 * not define.
 */
static int
_should_skip(struct xal *xal, struct xal_inode *dentry)
{
	struct xal_odf_dinode *dinode;
	uint16_t mode;
	int err;

	if (dentry->ftype == XAL_ODF_DIR3_FT_UNKNOWN) {
		err = dinodes_get(xal, dentry->ino, (void **)&dinode);
		if (err) {
			return err;
		}
		mode = be16toh(dinode->di_mode);
		if (S_ISDIR(mode)) {
			dentry->ftype = XAL_ODF_DIR3_FT_DIR;
		} else if (S_ISREG(mode)) {
			dentry->ftype = XAL_ODF_DIR3_FT_REG_FILE;
		}
	}

	if (dentry->ftype >= XAL_ODF_DIR3_FT_MAX) {
		XAL_DEBUG("FAILED: invalid ftype(%u) for ino(0x%" PRIx64 ")", dentry->ftype,
			  dentry->ino);
		return -EINVAL;
	}

	return dentry->ftype != XAL_ODF_DIR3_FT_DIR && dentry->ftype != XAL_ODF_DIR3_FT_REG_FILE;
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
	cand->index_state = &cand->_index_state_storage;
	cand->seq_lock = &cand->_seq_lock_storage;

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

#define FNV1A_BASIS 0xcbf29ce484222325ULL
#define FNV1A_PRIME 0x100000001b3ULL

static uint64_t
fnv1a(uint64_t hash, const uint8_t *buf, size_t nbytes)
{
	for (size_t i = 0; i < nbytes; ++i) {
		hash ^= buf[i];
		hash *= FNV1A_PRIME;
	}

	return hash;
}

/**
 * Take opts.subtree as the XFS backend indexes it: an absolute path from the root of the
 * filesystem, stored without repeated or trailing slashes, and NULL when it names the root
 *
 * @return On success, 0 is returned. -EINVAL is returned for a relative path or one with a '.' or
 * '..' component, -ENAMETOOLONG for one that does not fit XAL_PATH_MAXLEN, -ENOMEM when out of
 * memory.
 */
static int
subtree_normalize(const char *subtree, char **normalized)
{
	size_t len = subtree ? strlen(subtree) : 0, nbytes = 0;
	char *path;

	*normalized = NULL;
	if (!len) {
		return 0;
	}
	if (subtree[0] != '/') {
		XAL_DEBUG("FAILED: subtree(%s) is not an absolute path", subtree);
		return -EINVAL;
	}
	if (len >= XAL_PATH_MAXLEN) {
		XAL_DEBUG("FAILED: subtree(%s) is too long", subtree);
		return -ENAMETOOLONG;
	}

	path = malloc(len + 1);
	if (!path) {
		XAL_DEBUG("FAILED: malloc()");
		return -ENOMEM;
	}
	for (const char *cursor = subtree; *cursor;) {
		size_t clen;

		while (*cursor == '/') {
			++cursor;
		}
		clen = strcspn(cursor, "/");
		if (!clen) {
			break;
		}
		if ((clen == 1 && cursor[0] == '.') ||
		    (clen == 2 && cursor[0] == '.' && cursor[1] == '.')) {
			XAL_DEBUG("FAILED: subtree(%s) has a '.' or '..' component", subtree);
			free(path);
			return -EINVAL;
		}
		path[nbytes++] = '/';
		memcpy(path + nbytes, cursor, clen);
		nbytes += clen;
		cursor += clen;
	}
	path[nbytes] = '\0';

	if (!nbytes) {
		free(path);
		return 0;
	}
	*normalized = path;

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

	be->tree = calloc(1, sizeof(*be->tree));
	if (!be->tree) {
		XAL_DEBUG("FAILED: calloc()");
		err = -ENOMEM;
		goto failed;
	}
	be->tree->root_ino = cand->sb.rootino;

	err = subtree_normalize(opts->subtree, &be->tree->subtree);
	if (err) {
		XAL_DEBUG("FAILED: subtree_normalize(); err(%d)", err);
		goto failed;
	}

	be->tree->headers = FNV1A_BASIS;
	for (uint32_t seqno = 0; seqno < cand->sb.agcount; ++seqno) {
		err = retrieve_and_decode_allocation_group(dev, buf, seqno, cand);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_and_decode_allocation_group(inodes); err(%d)",
				  err);
			goto failed;
		}

		cand->sb.nallocated += be->ags[seqno].agi_count;

		/* The bytes index_key_get() hashes, as they are before any inode is read */
		be->tree->headers = fnv1a(be->tree->headers, buf, (size_t)cand->sb.sectsize * 4);
	}

	if (opts->shm_name) {
		if (strlen(opts->shm_name) > XAL_PATH_MAXLEN) {
			XAL_DEBUG("FAILED: shm_name too long");
			err = -EINVAL;
			goto failed;
		}

		cand->procrole = XAL_PROCROLE_PRIMARY;
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
	uint64_t ofz = xal_fsbno_offset(xal, fsbno);
	int err = -ENOSYS;

	XAL_DEBUG("ENTER: fsbno(0x%" PRIx64 ", %" PRIu64 ") @ ofz(%" PRIu64 ")", fsbno, fsbno, ofz);

	err = read_index_block(xal, ofz, xal->sb.blocksize, buf);
	if (err) {
		XAL_DEBUG("FAILED: read_index_block(); err(%d)", err);
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

			err = read_index_block(xal, ofz_disk, xal->sb.dirblocksize, dblock);
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
				err = _should_skip(xal, &dentry);
				if (err < 0) {
					return err;
				}
				if (err) {
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

	/* Slots are claimed as entries are kept, so the range starts at the next free one. */
	self->content.dentries.inodes_idx = xal->inodes.free;

	/** DECODE: namelen[1], offset[2], name[namelen], ftype[1], ino[4] | ino[8] */
	for (int i = 0; i < count; ++i) {
		struct xal_inode dentry = {0};
		uint32_t slot;

		dentry.namelen = *cursor;
		cursor += 1 + 2; ///< Advance past 'namelen' and 'offset[2]'

		memcpy(dentry.name, cursor, dentry.namelen);
		cursor += dentry.namelen; ///< Advance past 'name'

		dentry.ftype = *cursor;
		cursor += 1; ///< Advance past 'ftype'

		/**
		 * A non-zero 'i8count' means every inode number in the directory --
		 * including the parent above -- is 64-bit; otherwise all are 32-bit.
		 */
		if (i8count) {
			dentry.ino = be64toh(*(uint64_t *)cursor);
			cursor += 8; ///< Advance past 64-bit inode number
		} else {
			dentry.ino = be32toh(*(uint32_t *)cursor);
			cursor += 4; ///< Advance past 32-bit inode number
		}

		err = _should_skip(xal, &dentry);
		if (err < 0) {
			return err;
		}
		if (err) {
			continue;
		}

		err = xal_pool_claim_inodes(&xal->inodes, 1, &slot);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_claim_inodes(); err(%d)", err);
			return err;
		}
		*xal_inode_at(xal, slot) = dentry;
		self->content.dentries.count += 1;
	}

	{
		/* Sort the entries in the directory to allow for binary search */
		struct xal_inode *all_inodes = (struct xal_inode *)xal->inodes.memory;
		struct xal_inode *these_inodes = &all_inodes[self->content.dentries.inodes_idx];

		qsort(these_inodes, self->content.dentries.count, sizeof(struct xal_inode),
		      compare_inode);
	}

	for (uint32_t i = 0; i < self->content.dentries.count; ++i) {
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
	uint8_t dblock[ODF_BLOCK_FS_BYTES_MAX] = {0};
	union xal_odf_btree_magic *magic = (void *)(dblock);
	size_t ofz_disk = xal_fsbno_offset(xal, fsbno);
	int err;

	XAL_DEBUG("ENTER");

	err = read_index_block(xal, ofz_disk, xal->sb.dirblocksize, dblock);
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
		err = _should_skip(xal, &dentry);
		if (err < 0) {
			return err;
		}
		if (err) {
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

/* Where the leaf section of a directory starts, in bytes (XFS_DIR2_LEAF_OFFSET) */
#define DIR2_LEAF_OFFSET_NBYTES (32ULL << 30)

struct batch_items {
	struct batch_item *items;
	size_t nitems;
	size_t capacity;
	size_t limit; ///< Most items the list takes; adding one more fails with -E2BIG
};

static int
batch_items_add(struct batch_items *list, uint64_t ofz, uint32_t nbytes)
{
	if (list->nitems >= list->limit) {
		XAL_DEBUG("FAILED: more than %zu blocks to read ahead", list->limit);
		return -E2BIG;
	}
	if (list->nitems == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 1024;
		struct batch_item *grown = realloc(list->items, capacity * sizeof(*grown));

		if (!grown) {
			XAL_DEBUG("FAILED: realloc()");
			return -ENOMEM;
		}
		list->items = grown;
		list->capacity = capacity;
	}
	list->items[list->nitems] = (struct batch_item){
	    .ofz = ofz,
	    .nbytes = nbytes,
	    .idx = list->nitems,
	};
	list->nitems += 1;

	return 0;
}

/* Directories to read the blocks of, or whose entries to read the inodes of */
struct dinode_list {
	struct xal_odf_dinode **dinodes;
	size_t ndinodes;
	size_t capacity;
};

static int
dinode_list_add(struct dinode_list *list, struct xal_odf_dinode *dinode)
{
	if (list->ndinodes == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 64;
		struct xal_odf_dinode **grown = realloc(list->dinodes, capacity * sizeof(*grown));

		if (!grown) {
			XAL_DEBUG("FAILED: realloc()");
			return -ENOMEM;
		}
		list->dinodes = grown;
		list->capacity = capacity;
	}
	list->dinodes[list->ndinodes++] = dinode;

	return 0;
}

/**
 * Keep the block read for 'item' at its index in the area 'arg'; a batch_item_fn
 */
static int
dirblocks_store(struct xal *xal, const struct batch_item *item, const uint8_t *data, void *arg)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks = be->dirblocks;
	uint8_t *block = (uint8_t *)arg + (size_t)item->idx * xal->sb.dirblocksize;
	khiter_t iter;
	int ret;

	memcpy(block, data, item->nbytes);

	iter = kh_put(ofz_to_block, dirblocks->map, item->ofz, &ret);
	if (ret < 0) {
		XAL_DEBUG("FAILED: kh_put()");
		return -ENOMEM;
	}
	kh_value(dirblocks->map, iter) = (struct dirblock){.data = block, .nbytes = item->nbytes};

	return 0;
}

static int
dirblocks_read_round(struct xal *xal, struct batch_items *list)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks = be->dirblocks;
	uint8_t **areas, *area;

	if (!list->nitems) {
		return 0;
	}

	areas = realloc(dirblocks->areas, (dirblocks->nareas + 1) * sizeof(*areas));
	if (!areas) {
		XAL_DEBUG("FAILED: realloc()");
		return -ENOMEM;
	}
	dirblocks->areas = areas;

	area = malloc(list->nitems * (size_t)xal->sb.dirblocksize);
	if (!area) {
		XAL_DEBUG("FAILED: malloc()");
		return -ENOMEM;
	}
	dirblocks->areas[dirblocks->nareas++] = area;

	return batch_read(xal, list->items, list->nitems, dirblocks_store, area);
}

static void
dirblocks_free(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks = be->dirblocks;

	if (!dirblocks) {
		return;
	}

	if (dirblocks->map) {
		kh_destroy(ofz_to_block, dirblocks->map);
	}
	for (size_t i = 0; i < dirblocks->nareas; ++i) {
		free(dirblocks->areas[i]);
	}
	free(dirblocks->areas);
	free(dirblocks);
	be->dirblocks = NULL;
}

static int
dirblocks_alloc(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks;

	dirblocks_free(xal);

	dirblocks = calloc(1, sizeof(*dirblocks));
	if (!dirblocks) {
		XAL_DEBUG("FAILED: calloc()");
		return -ENOMEM;
	}
	dirblocks->map = kh_init(ofz_to_block);
	if (!dirblocks->map) {
		XAL_DEBUG("FAILED: kh_init()");
		free(dirblocks);
		return -ENOMEM;
	}
	be->dirblocks = dirblocks;

	return 0;
}

/**
 * Add to 'blocks' what the inode of the directory 'dinode' tells of its blocks
 *
 * For an extents-format directory these are the blocks its extents list up to its size, as
 * process_dinode_dir_extents() reads them. For a btree-format directory whose root is at level 1
 * they are the bmbt leaves the root points at, which also go to 'leaves' for
 * dirblocks_gather_leaves(). Anything else adds nothing; that includes an inode that is not a
 * directory, or not a consistent one, as the inodes may be ones the walk does not reach.
 */
static int
dirblocks_gather(struct xal *xal, struct xal_odf_dinode *dinode, struct batch_items *blocks,
		 struct batch_items *leaves)
{
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	const size_t litino = xal->sb.inodesize - sizeof(struct xal_odf_dinode);
	void *dfork = ((uint8_t *)dinode) + sizeof(*dinode);
	size_t dsize;
	int err = 0;

	if (be16toh(dinode->di_magic) != XAL_DINODE_MAGIC || !S_ISDIR(be16toh(dinode->di_mode))) {
		return 0;
	}

	/* The size of the data fork, as XFS_DFORK_DSIZE() gives it */
	dsize = dinode->di_forkoff ? dinode->di_forkoff * 8UL : litino;
	if (dsize > litino) {
		return 0;
	}

	if (dinode->di_format == XAL_DINODE_FMT_EXTENTS) {
		struct pair_u64 *extents = dfork;
		uint64_t nextents = be32toh(dinode->di_nextents);
		int64_t nbytes = be64toh(dinode->size);

		if (!nextents) {
			nextents = be64toh(dinode->di_big_nextents);
		}
		if (nextents > dsize / sizeof(*extents)) {
			return 0;
		}
		for (uint64_t i = 0; (nbytes > 0) && (i < nextents) && !err; ++i) {
			struct xal_extent extent = {0};

			decode_xfs_extent(be64toh(extents[i].l0), be64toh(extents[i].l1), &extent);
			/* Up to the directory's size, which on a consistent directory is where the
			 * walk stops too, at the end of an extent */
			for (size_t fsblk = 0;
			     fsblk < extent.nblocks &&
			     (int64_t)(fsblk * xal->sb.blocksize) < nbytes && !err;
			     fsblk += fsblk_per_dblk) {
				err = batch_items_add(
				    blocks, xal_fsbno_offset(xal, extent.start_block + fsblk),
				    xal->sb.dirblocksize);
			}
			nbytes -= extent.nblocks * xal->sb.blocksize;
		}
	} else if (dinode->di_format == XAL_DINODE_FMT_BTREE &&
		   be16toh(((uint16_t *)dfork)[0]) == 1) {
		/* A header, the keys, then the pointers, as in xfs_bmdr_maxrecs() */
		size_t rootrecs = (dsize - 4) / 16;
		uint16_t numrecs = be16toh(((uint16_t *)dfork)[1]);
		const uint8_t *fsbnos = (uint8_t *)dfork + 4 + rootrecs * 8;

		if (!numrecs || numrecs > rootrecs) {
			return 0;
		}
		for (uint16_t rec = 0; rec < numrecs && !err; ++rec) {
			uint64_t fsbno, ofz;

			/* The pointers sit at a 4-byte offset of the 8-byte aligned fork */
			memcpy(&fsbno, fsbnos + rec * sizeof(fsbno), sizeof(fsbno));
			ofz = xal_fsbno_offset(xal, be64toh(fsbno));
			err = batch_items_add(leaves, ofz, xal->sb.blocksize);
			if (!err) {
				err = batch_items_add(blocks, ofz, xal->sb.blocksize);
			}
		}
	}

	return err;
}

/**
 * Add to 'blocks' the blocks of the data sections that the bmbt leaves in 'leaves' list, once a
 * round has read the leaves
 */
static int
dirblocks_gather_leaves(struct xal *xal, struct batch_items *leaves, struct batch_items *blocks)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dirblocks *dirblocks = be->dirblocks;
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	const uint64_t leaf_fsblk = DIR2_LEAF_OFFSET_NBYTES / xal->sb.blocksize;
	const size_t maxrecs =
	    (xal->sb.blocksize - sizeof(struct xal_odf_btree_lfmt)) / sizeof(struct pair_u64);
	int err = 0;

	for (size_t i = 0; i < leaves->nitems && !err; ++i) {
		khiter_t iter = kh_get(ofz_to_block, dirblocks->map, leaves->items[i].ofz);
		struct xal_odf_btree_lfmt *leaf;
		struct pair_u64 *pairs;

		if (iter == kh_end(dirblocks->map)) {
			continue;
		}
		leaf = (void *)kh_value(dirblocks->map, iter).data;
		pairs = (void *)(((uint8_t *)leaf) + sizeof(*leaf));
		if (be32toh(leaf->magic.num) != XAL_ODF_BMAP_CRC_MAGIC ||
		    be16toh(leaf->pos.level) || be16toh(leaf->pos.numrecs) > maxrecs) {
			continue;
		}

		for (uint16_t rec = 0; rec < be16toh(leaf->pos.numrecs) && !err; ++rec) {
			struct xal_extent extent = {0};

			decode_xfs_extent(be64toh(pairs[rec].l0), be64toh(pairs[rec].l1), &extent);
			/* The records are sorted by offset; the rest are of the leaf section */
			if (extent.start_offset >= leaf_fsblk) {
				break;
			}
			for (size_t fsblk = 0; fsblk < extent.nblocks && !err;
			     fsblk += fsblk_per_dblk) {
				err = batch_items_add(
				    blocks, xal_fsbno_offset(xal, extent.start_block + fsblk),
				    xal->sb.dirblocksize);
			}
		}
	}

	return err;
}

/**
 * Read ahead the blocks of the directories in 'dirs': first those their inodes list, then those
 * the bmbt leaves among them list, until 'limit' blocks are held
 */
static int
dirblocks_read_dirs(struct xal *xal, struct dinode_list *dirs, size_t limit)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	size_t held = kh_size(((struct dirblocks *)be->dirblocks)->map);
	struct batch_items blocks = {0}, leaves = {0};
	int err = 0;

	blocks.limit = leaves.limit = held < limit ? limit - held : 0;

	for (size_t i = 0; i < dirs->ndinodes && !err; ++i) {
		err = dirblocks_gather(xal, dirs->dinodes[i], &blocks, &leaves);
	}
	if (!err) {
		err = dirblocks_read_round(xal, &blocks);
	}

	/* The leaves are in memory now; gather the blocks they list for a second round */
	blocks.limit -= blocks.nitems;
	blocks.nitems = 0;
	if (!err) {
		err = dirblocks_gather_leaves(xal, &leaves, &blocks);
	}
	if (!err) {
		err = dirblocks_read_round(xal, &blocks);
	}

	free(blocks.items);
	free(leaves.items);

	return err;
}

/**
 * Read ahead the blocks process_ino() will read for the directories, so that the index finds them
 * in memory
 *
 * These are the blocks dirblocks_gather() and dirblocks_gather_leaves() add, for every directory
 * in the inobt. Blocks not read here are read when the index asks for them: the first block of a
 * leaf section, which the walk reads to find where the data section ends, and the bmbt blocks of
 * files.
 */
static int
dirblocks_read(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dinodes_index *index = be->dinodes_map;
	/* Hold no more bytes of directory blocks than be->dinodes holds of inodes */
	const size_t limit = xal->sb.nallocated * xal->sb.inodesize / xal->sb.dirblocksize;
	struct dinode_list dirs = {0};
	int err;

	err = dirblocks_alloc(xal);
	if (err) {
		return err;
	}

	for (size_t i = 0; i < index->ndinodes && !err; ++i) {
		struct xal_odf_dinode *dinode = (void *)&be->dinodes[i * xal->sb.inodesize];

		if (S_ISDIR(be16toh(dinode->di_mode))) {
			err = dinode_list_add(&dirs, dinode);
		}
	}
	if (!err) {
		err = dirblocks_read_dirs(xal, &dirs, limit);
	}

	free(dirs.dinodes);
	if (err) {
		dirblocks_free(xal);
	}

	return err;
}

/**
 * Called by dir_entries() with each entry of a directory; a non-zero return stops it and is what
 * dir_entries() returns
 */
typedef int (*dir_entry_fn)(struct xal *xal, struct xal_inode *dentry, void *arg);

/* The entries of a data or block format directory block, as process_dinode_dir_extents_dblock()
 * decodes them */
static int
dir_block_entries(struct xal *xal, uint64_t ofz, dir_entry_fn fn, void *arg)
{
	/* Room past the block for decode_dentry() to read a last, truncated entry from */
	uint8_t dblock[ODF_BLOCK_DIR_BYTES_MAX + 512];
	union xal_odf_btree_magic *magic = (void *)dblock;
	int err;

	err = read_index_block(xal, ofz, xal->sb.dirblocksize, dblock);
	if (err) {
		XAL_DEBUG("FAILED: read_index_block(); err(%d)", err);
		return err;
	}
	memset(dblock + xal->sb.dirblocksize, 0, 512);

	if ((be32toh(magic->num) != XAL_ODF_DIR3_DATA_MAGIC) &&
	    (be32toh(magic->num) != XAL_ODF_DIR3_BLOCK_MAGIC)) {
		return 0;
	}

	for (uint64_t cursor = 64; cursor < xal->sb.dirblocksize;) {
		struct xal_inode dentry = {0};
		int nbytes = decode_dentry(dblock + cursor, &dentry);

		if (nbytes <= 0 || !dentry.ino || !dentry.namelen) {
			break;
		}
		cursor += nbytes;

		if ((dentry.namelen == 1 && dentry.name[0] == '.') ||
		    (dentry.namelen == 2 && dentry.name[0] == '.' && dentry.name[1] == '.')) {
			continue;
		}
		err = fn(xal, &dentry, arg);
		if (err) {
			return err;
		}
	}

	return 0;
}

/* The entries of a shortform directory, as process_dinode_dir_local() decodes them */
static int
dir_local_entries(struct xal *xal, const uint8_t *dfork, size_t dsize, dir_entry_fn fn, void *arg)
{
	const uint8_t *cursor = dfork, *end = dfork + dsize;
	uint8_t count, i8count;
	size_t ino_nbytes;
	int err;

	if (dsize < 6) {
		return -EUCLEAN;
	}
	count = cursor[0];
	i8count = cursor[1];
	ino_nbytes = i8count ? 8 : 4;
	cursor += 2 + ino_nbytes; ///< Past the counts and the parent inode number

	for (int i = 0; i < count; ++i) {
		struct xal_inode dentry = {0};

		if (cursor + 3 > end || cursor + 3 + cursor[0] + 1 + ino_nbytes > end) {
			return -EUCLEAN;
		}
		dentry.namelen = cursor[0];
		cursor += 1 + 2; ///< Past 'namelen' and 'offset[2]'
		memcpy(dentry.name, cursor, dentry.namelen);
		cursor += dentry.namelen;
		dentry.ftype = *cursor;
		cursor += 1;
		if (i8count) {
			uint64_t ino;

			memcpy(&ino, cursor, sizeof(ino));
			dentry.ino = be64toh(ino);
		} else {
			uint32_t ino;

			memcpy(&ino, cursor, sizeof(ino));
			dentry.ino = be32toh(ino);
		}
		cursor += ino_nbytes;

		err = fn(xal, &dentry, arg);
		if (err) {
			return err;
		}
	}

	return 0;
}

/* The entries of a btree-format directory whose root is at level 1, in the data blocks its bmbt
 * leaves list ahead of the leaf section */
static int
dir_btree_entries(struct xal *xal, const uint8_t *dfork, size_t dsize, dir_entry_fn fn, void *arg)
{
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	const uint64_t leaf_fsblk = DIR2_LEAF_OFFSET_NBYTES / xal->sb.blocksize;
	const size_t maxrecs =
	    (xal->sb.blocksize - sizeof(struct xal_odf_btree_lfmt)) / sizeof(struct pair_u64);
	/* A header, the keys, then the pointers, as in xfs_bmdr_maxrecs() */
	size_t rootrecs = (dsize - 4) / 16;
	uint16_t level, numrecs;
	int err = 0;

	memcpy(&level, dfork, sizeof(level));
	memcpy(&numrecs, dfork + 2, sizeof(numrecs));
	level = be16toh(level);
	numrecs = be16toh(numrecs);
	if (level > 1) {
		return -ENOSYS; ///< As btree_lblock_decode_node_records()
	}
	if (level != 1 || !numrecs || numrecs > rootrecs) {
		return -EUCLEAN;
	}

	for (uint16_t rec = 0; rec < numrecs && !err; ++rec) {
		uint8_t block[ODF_BLOCK_FS_BYTES_MAX];
		struct xal_odf_btree_lfmt *leaf = (void *)block;
		struct pair_u64 *pairs = (void *)(block + sizeof(*leaf));
		uint64_t fsbno;

		memcpy(&fsbno, dfork + 4 + rootrecs * 8 + rec * sizeof(fsbno), sizeof(fsbno));
		err = read_index_block(xal, xal_fsbno_offset(xal, be64toh(fsbno)),
				       xal->sb.blocksize, block);
		if (err) {
			XAL_DEBUG("FAILED: read_index_block(); err(%d)", err);
			return err;
		}
		if (be32toh(leaf->magic.num) != XAL_ODF_BMAP_CRC_MAGIC ||
		    be16toh(leaf->pos.level) || be16toh(leaf->pos.numrecs) > maxrecs) {
			return -EUCLEAN;
		}

		for (uint16_t i = 0; i < be16toh(leaf->pos.numrecs) && !err; ++i) {
			struct xal_extent extent = {0};

			decode_xfs_extent(be64toh(pairs[i].l0), be64toh(pairs[i].l1), &extent);
			if (extent.start_offset >= leaf_fsblk) {
				break;
			}
			for (size_t fsblk = 0; fsblk < extent.nblocks && !err;
			     fsblk += fsblk_per_dblk) {
				err = dir_block_entries(
				    xal, xal_fsbno_offset(xal, extent.start_block + fsblk), fn,
				    arg);
			}
		}
	}

	return err;
}

/**
 * Hand 'fn' the entries of the directory 'dinode', but '.' and '..', from the blocks the walk
 * reads for it, through read_index_block()
 *
 * On a consistent directory these are the entries process_ino() indexes. A data section is read
 * only up to the directory's size. A btree root above level 1, which the walk refuses too, gives
 * -ENOSYS, and a directory that is not consistent -EUCLEAN.
 */
static int
dir_entries(struct xal *xal, struct xal_odf_dinode *dinode, dir_entry_fn fn, void *arg)
{
	const uint32_t fsblk_per_dblk = xal->sb.dirblocksize / xal->sb.blocksize;
	const size_t litino = xal->sb.inodesize - sizeof(*dinode);
	uint8_t *dfork = (uint8_t *)dinode + sizeof(*dinode);
	size_t dsize = dinode->di_forkoff ? dinode->di_forkoff * 8UL : litino;
	struct pair_u64 *extents = (void *)dfork;
	uint64_t nextents = be32toh(dinode->di_nextents);
	int64_t nbytes = be64toh(dinode->size);
	int err = 0;

	if (dsize > litino) {
		return -EUCLEAN;
	}

	switch (dinode->di_format) {
	case XAL_DINODE_FMT_LOCAL:
		return dir_local_entries(xal, dfork, dsize, fn, arg);

	case XAL_DINODE_FMT_BTREE:
		return dir_btree_entries(xal, dfork, dsize, fn, arg);

	case XAL_DINODE_FMT_EXTENTS:
		if (!nextents) {
			nextents = be64toh(dinode->di_big_nextents);
		}
		if (nextents > dsize / sizeof(*extents)) {
			return -EUCLEAN;
		}
		for (uint64_t i = 0; (nbytes > 0) && (i < nextents) && !err; ++i) {
			struct xal_extent extent = {0};

			decode_xfs_extent(be64toh(extents[i].l0), be64toh(extents[i].l1), &extent);
			for (size_t fsblk = 0;
			     fsblk < extent.nblocks &&
			     (int64_t)(fsblk * xal->sb.blocksize) < nbytes && !err;
			     fsblk += fsblk_per_dblk) {
				err = dir_block_entries(
				    xal, xal_fsbno_offset(xal, extent.start_block + fsblk), fn,
				    arg);
			}
			nbytes -= extent.nblocks * xal->sb.blocksize;
		}
		return err;
	}

	return -EUCLEAN;
}

/**
 * The byte offset of the inode 'ino' on the device, refusing one outside the allocation groups
 */
static int
dinode_ofz(struct xal *xal, uint64_t ino, uint64_t *ofz)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	uint64_t agno = ino >> (xal->sb.inopblog + xal->sb.agblklog);
	uint64_t agbno = (ino >> xal->sb.inopblog) & ((1ULL << xal->sb.agblklog) - 1);

	if (agno >= xal->sb.agcount || agbno >= be->ags[agno].agf_length) {
		XAL_DEBUG("FAILED: ino(0x%" PRIx64 ") is outside the filesystem", ino);
		return -EINVAL;
	}
	*ofz = xal_ino_decode_absolute_offset(xal, ino);

	return 0;
}

/* The read that brings in the inode 'ino' for the item 'idx': the inode, or the LBA it is in when
 * that is larger */
static int
dinode_item(struct xal *xal, uint64_t ino, uint32_t idx, struct batch_item *item)
{
	const struct xnvme_geo *geo = xnvme_dev_get_geo(xal->dev);
	uint32_t nbytes = xal->sb.inodesize > geo->lba_nbytes ? xal->sb.inodesize : geo->lba_nbytes;
	uint64_t ofz;
	int err;

	err = dinode_ofz(xal, ino, &ofz);
	if (err) {
		return err;
	}
	*item = (struct batch_item){.ofz = ofz - ofz % nbytes, .nbytes = nbytes, .idx = idx};

	return 0;
}

/* The inodes dinodes_read() reads, and where it lists the directories among them */
struct dinodes_read_args {
	const uint64_t *inos;
	struct dinode_list *dirs;
};

/**
 * Keep the inode read for 'item' in be->dinodes, unless it is there already; a batch_item_fn
 *
 * Without the inobt to tell which inodes are allocated, an inode is taken only when it has the
 * magic, a version 3 core with the number of the entry it was read for, and a mode.
 */
static int
dinode_store(struct xal *xal, const struct batch_item *item, const uint8_t *data, void *arg)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct dinodes_index *index = be->dinodes_map;
	khash_t(ino_to_dinode) *dinodes_map = index->map;
	struct dinodes_read_args *args = arg;
	uint64_t ino = args->inos[item->idx];
	const struct xal_odf_dinode *src;
	struct xal_odf_dinode *dinode;
	size_t ndinodes = index->ndinodes;
	uint64_t ofz;
	khiter_t iter;
	int ret;

	if (kh_get(ino_to_dinode, dinodes_map, ino) != kh_end(dinodes_map)) {
		return 0;
	}

	ret = dinode_ofz(xal, ino, &ofz);
	if (ret) {
		return ret;
	}
	src = (const void *)(data + (ofz - item->ofz));
	if (be16toh(src->di_magic) != XAL_DINODE_MAGIC || src->di_version != 3 ||
	    be64toh(src->ino) != ino || !be16toh(src->di_mode)) {
		XAL_DEBUG("FAILED: no allocated inode at ino(0x%" PRIx64 ")", ino);
		return -EUCLEAN;
	}
	if (ndinodes == xal->sb.nallocated) {
		XAL_DEBUG("FAILED: more inodes than the AGIs count(%" PRIu64 ")",
			  xal->sb.nallocated);
		return -EINVAL;
	}

	dinode = (void *)&be->dinodes[ndinodes * xal->sb.inodesize];
	memcpy(dinode, src, xal->sb.inodesize);

	iter = kh_put(ino_to_dinode, dinodes_map, ino, &ret);
	if (ret < 0) {
		XAL_DEBUG("FAILED: kh_put()");
		return -ENOMEM;
	}
	kh_value(dinodes_map, iter) = dinode;
	index->ndinodes += 1;

	if (args->dirs && S_ISDIR(be16toh(dinode->di_mode))) {
		return dinode_list_add(args->dirs, dinode);
	}

	return 0;
}

/**
 * Read the 'ninos' inodes in 'inos' into be->dinodes with batch_read(), each with the block it is
 * in, and list in 'dirs' the directories among them
 */
static int
dinodes_read(struct xal *xal, const uint64_t *inos, size_t ninos, struct dinode_list *dirs)
{
	struct dinodes_read_args args = {.inos = inos, .dirs = dirs};
	struct batch_item *items;
	int err = 0;

	if (!ninos) {
		return 0;
	}
	if (ninos > UINT32_MAX) {
		return -EINVAL;
	}

	items = calloc(ninos, sizeof(*items));
	if (!items) {
		XAL_DEBUG("FAILED: calloc()");
		return -ENOMEM;
	}
	for (size_t i = 0; i < ninos && !err; ++i) {
		err = dinode_item(xal, inos[i], i, &items[i]);
	}
	if (!err) {
		err = batch_read(xal, items, ninos, dinode_store, &args);
	}

	free(items);

	return err;
}

/**
 * Find the inode 'ino' in be->dinodes, reading it from the device into there when it is not
 */
static int
dinode_read(struct xal *xal, uint64_t ino, struct xal_odf_dinode **dinode)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	khash_t(ino_to_dinode) *dinodes_map = ((struct dinodes_index *)be->dinodes_map)->map;
	struct dinodes_read_args args = {.inos = &ino};
	struct batch_item item;
	khiter_t iter;
	int err;

	iter = kh_get(ino_to_dinode, dinodes_map, ino);
	if (iter == kh_end(dinodes_map)) {
		err = dinode_item(xal, ino, 0, &item);
		if (err) {
			return err;
		}

		err = dev_read(xal->dev, be->buf, item.nbytes, item.ofz);
		if (err) {
			XAL_DEBUG("FAILED: dev_read(); err(%d)", err);
			return err;
		}
		err = dinode_store(xal, &item, be->buf, &args);
		if (err) {
			return err;
		}
		iter = kh_get(ino_to_dinode, dinodes_map, ino);
	}
	*dinode = kh_value(dinodes_map, iter);

	return 0;
}

/* The entry subtree_resolve() looks for in a directory */
struct dir_lookup {
	const char *name;
	size_t namelen;
	uint64_t ino;
};

static int
dir_lookup_entry(struct xal *XAL_UNUSED(xal), struct xal_inode *dentry, void *arg)
{
	struct dir_lookup *lookup = arg;

	if (dentry->namelen != lookup->namelen ||
	    memcmp(dentry->name, lookup->name, lookup->namelen)) {
		return 0;
	}
	lookup->ino = dentry->ino;

	return 1;
}

/**
 * Look up the subtree one component at a time from the root directory, and set its inode as the
 * root of the index
 *
 * @return On success, 0 is returned. -ENOENT is returned when a component is not there,
 * -ENOTDIR when one is not a directory; other negative errno on error.
 */
static int
subtree_resolve(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct xal_odf_dinode *dinode;
	int err;

	err = dinode_read(xal, xal->sb.rootino, &dinode);
	if (err) {
		return err;
	}

	for (const char *cursor = be->tree->subtree; *cursor;) {
		struct dir_lookup lookup = {0};

		if (!S_ISDIR(be16toh(dinode->di_mode))) {
			return -ENOTDIR;
		}
		lookup.name = cursor + 1;
		lookup.namelen = strcspn(lookup.name, "/");

		err = dir_entries(xal, dinode, dir_lookup_entry, &lookup);
		if (err < 0) {
			return err;
		}
		if (!err) {
			XAL_DEBUG("FAILED: no '%.*s' in subtree(%s)", (int)lookup.namelen,
				  lookup.name, be->tree->subtree);
			return -ENOENT;
		}
		err = dinode_read(xal, lookup.ino, &dinode);
		if (err) {
			return err;
		}
		cursor = lookup.name + lookup.namelen;
	}
	if (!S_ISDIR(be16toh(dinode->di_mode))) {
		return -ENOTDIR;
	}
	be->tree->root_ino = be64toh(dinode->ino);

	return 0;
}

/* The inodes of the entries of one level of the subtree */
struct ino_list {
	uint64_t *inos;
	size_t ninos;
	size_t capacity;
};

/* List an entry the walk indexes, and whose inode is not read yet; a dir_entry_fn */
static int
subtree_entry(struct xal *xal, struct xal_inode *dentry, void *arg)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	khash_t(ino_to_dinode) *dinodes_map = ((struct dinodes_index *)be->dinodes_map)->map;
	struct ino_list *list = arg;

	if (dentry->ftype != XAL_ODF_DIR3_FT_UNKNOWN && dentry->ftype != XAL_ODF_DIR3_FT_DIR &&
	    dentry->ftype != XAL_ODF_DIR3_FT_REG_FILE) {
		return 0;
	}
	if (kh_get(ino_to_dinode, dinodes_map, dentry->ino) != kh_end(dinodes_map)) {
		return 0;
	}
	if (list->ninos == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 1024;
		uint64_t *grown = realloc(list->inos, capacity * sizeof(*grown));

		if (!grown) {
			XAL_DEBUG("FAILED: realloc()");
			return -ENOMEM;
		}
		list->inos = grown;
		list->capacity = capacity;
	}
	list->inos[list->ninos++] = dentry->ino;

	return 0;
}

/**
 * Read the inodes of the subtree into be->dinodes, and the blocks of its directories into
 * be->dirblocks, level by level
 *
 * The directory of the subtree is looked up from the root, and an error there is returned. Below
 * it this is a read-ahead of what the walk of xal_index() will ask for: each level's directory
 * blocks, in two rounds as dirblocks_read() reads them, then the inodes of their entries, with
 * batch_read(). When the blocks of a level are not read ahead, its entries are read from the
 * device as they come. Any other error stops it, and the walk reads the rest one at a time,
 * through dinodes_get() and read_index_block(), and returns the error when it is still there.
 */
static int
subtree_read(struct xal *xal)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	/* As dirblocks_read() bounds them */
	const size_t limit = xal->sb.nallocated * xal->sb.inodesize / xal->sb.dirblocksize;
	struct dinode_list level = {0}, next = {0};
	struct ino_list entries = {0};
	struct xal_odf_dinode *dinode;
	int err;

	err = dirblocks_alloc(xal);
	if (err) {
		return err;
	}

	err = subtree_resolve(xal);
	if (err) {
		XAL_DEBUG("FAILED: subtree_resolve(%s); err(%d)", be->tree->subtree, err);
		return err;
	}
	err = dinode_read(xal, be->tree->root_ino, &dinode);
	if (!err) {
		err = dinode_list_add(&level, dinode);
	}

	while (!err && level.ndinodes) {
		struct dinode_list swap;

		err = dirblocks_read_dirs(xal, &level, limit);
		if (err) {
			XAL_DEBUG(
			    "INFO: dirblocks_read_dirs(); err(%d), reading the blocks as they come",
			    err);
			err = 0;
		}

		entries.ninos = 0;
		for (size_t i = 0; i < level.ndinodes && !err; ++i) {
			err = dir_entries(xal, level.dinodes[i], subtree_entry, &entries);
		}

		next.ndinodes = 0;
		if (!err) {
			err = dinodes_read(xal, entries.inos, entries.ninos, &next);
		}

		swap = level;
		level = next;
		next = swap;
	}
	if (err) {
		XAL_DEBUG("INFO: subtree read-ahead stopped; err(%d), the walk reads the rest",
			  err);
	}

	free(level.dinodes);
	free(next.dinodes);
	free(entries.inos);

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

	/* Before the pools are cleared, and best effort: the walk reads what is not read ahead.
	 * With a subtree, xal_dinodes_retrieve() read its blocks along with its inodes. */
	if (!be->dirblocks) {
		err = dirblocks_read(xal);
		if (err) {
			XAL_DEBUG(
			    "INFO: dirblocks_read(); err(%d), reading the blocks one at a time",
			    err);
		}
	}

	atomic_store(xal->index_state, XAL_STATE_INDEXING);

	/* Bracket the rewrite with the sequence lock, as the FIEMAP backend does: the pools are
	 * cleared and refilled in place below, and a secondary mapping them has no other way to
	 * learn its extents were replaced. See xal_get_extents() for the protocol. */
	atomic_fetch_add(xal->seq_lock, 1);

	xal_pool_clear(&xal->inodes);
	xal_pool_clear(&xal->extents);

	err = xal_pool_claim_inodes(&xal->inodes, 1, &xal->root_idx);
	if (err) {
		goto exit;
	}

	root = xal_inode_at(xal, xal->root_idx);
	root->ino = be->tree->root_ino;
	root->ftype = XAL_ODF_DIR3_FT_DIR;
	root->namelen = 0;
	root->parent_idx = XAL_POOL_IDX_NONE;
	root->content.extents.count = 0;
	root->content.dentries.count = 0;

	err = process_ino(xal, root->ino, root);
	if (err) {
		XAL_DEBUG("FAILED: process_ino(); err(%d)", err);
	}

exit:
	dirblocks_free(xal);
	atomic_fetch_add(xal->seq_lock, 1);
	xal_mark_index_done(xal, err);

	return err;
}

/* What xal_index_save() records of the filesystem's state, and xal_index_load() compares */
struct index_key {
	uint8_t uuid[16]; ///< sb_uuid
	uint64_t log_lsn; ///< LSN of the unmount record at the head of the log
	uint32_t log_crc; ///< Its CRC
	uint32_t _rsvd;
	uint64_t headers; ///< FNV-1a of each AG's four header sectors, the primary superblock first
};

#define INDEX_FILE_MAGIC "XALINDEX"
#define INDEX_FILE_VERSION 1

/* What xal_index_save() writes ahead of the inode and extent pools */
struct index_file_hdr {
	char magic[8];		///< INDEX_FILE_MAGIC
	uint32_t version;	///< INDEX_FILE_VERSION
	uint32_t shm_version;	///< XAL_SHM_VERSION, which also covers what the pools mean
	uint32_t hdr_nbytes;	///< sizeof(struct index_file_hdr)
	uint32_t inode_nbytes;	///< sizeof(struct xal_inode)
	uint32_t extent_nbytes; ///< sizeof(struct xal_extent)
	uint32_t root_idx;
	uint64_t ninodes;
	uint64_t nextents;
	uint64_t root_ino;
	uint64_t sum;			   ///< pools_sum() of the inodes, then the extents
	char subtree[XAL_PATH_MAXLEN + 1]; ///< Empty for the whole filesystem
	struct index_key key;
};

/* The tid that xfsprogs gives the unmount record it writes when it formats a log */
#define LOG_XFSPROGS_TID 0xb0c0d0d0

/* A checksum of 'nbytes' at 'buf', eight bytes at a time in four lanes, for the pools */
static uint64_t
pools_sum(uint64_t sum, const uint8_t *buf, size_t nbytes)
{
	uint64_t lanes[4] = {sum, sum + 1, sum + 2, sum + 3};
	size_t i = 0;

	for (; i + sizeof(lanes) <= nbytes; i += sizeof(lanes)) {
		for (int l = 0; l < 4; ++l) {
			uint64_t word;

			memcpy(&word, buf + i + l * sizeof(word), sizeof(word));
			lanes[l] = (lanes[l] ^ word) * FNV1A_PRIME;
		}
	}

	return fnv1a(fnv1a(sum, (uint8_t *)lanes, sizeof(lanes)), buf + i, nbytes - i);
}

/* Copy the log block 'bb' of the log at 'log_ofz' into 'out', reading the LBA it is in */
static int
log_bb_read(struct xal *xal, uint64_t log_ofz, uint64_t bb, uint8_t *out)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	const struct xnvme_geo *geo = xnvme_dev_get_geo(xal->dev);
	uint64_t ofz = log_ofz + bb * XAL_ODF_LOG_BBSIZE;
	uint64_t lba_ofz = ofz - ofz % geo->lba_nbytes;
	int err;

	err = dev_read(xal->dev, be->buf, geo->lba_nbytes, lba_ofz);
	if (err) {
		XAL_DEBUG("FAILED: dev_read(); err(%d)", err);
		return err;
	}
	memcpy(out, (uint8_t *)be->buf + (ofz - lba_ofz), XAL_ODF_LOG_BBSIZE);

	return 0;
}

static uint32_t
log_bb_be32(const uint8_t *bb, size_t ofz)
{
	uint32_t val;

	memcpy(&val, bb + ofz, sizeof(val));

	return be32toh(val);
}

/* The cycle stamped on the log block 'bb', as xlog_get_cycle() reads it */
static int
log_bb_cycle(struct xal *xal, uint64_t log_ofz, uint64_t bb, uint32_t *cycle)
{
	uint8_t block[XAL_ODF_LOG_BBSIZE];
	int err;

	err = log_bb_read(xal, log_ofz, bb, block);
	if (err) {
		return err;
	}
	*cycle = log_bb_be32(block, 0) == XAL_ODF_LOG_HEADER_MAGIC
		     ? log_bb_be32(block, XAL_ODF_LOG_H_CYCLE)
		     : log_bb_be32(block, 0);

	return 0;
}

/**
 * Find the head of a log of 'nbbs' blocks at 'log_ofz': the first block of the latest cycle's
 * end, as xlog_find_head() finds it in a log without holes from torn writes
 *
 * Block 0 carries the cycle 'cycle'. The head is where the cycle drops by one, or turns to zero
 * in a log not yet written to its end, or block 0 when the whole log carries one cycle.
 */
static int
log_find_head(struct xal *xal, uint64_t log_ofz, uint64_t nbbs, uint64_t *head, uint32_t *cycle)
{
	uint32_t first, last, mid_cycle;
	uint64_t lo = 0, hi = nbbs - 1;
	int err;

	err = log_bb_cycle(xal, log_ofz, 0, &first);
	if (!err) {
		err = log_bb_cycle(xal, log_ofz, nbbs - 1, &last);
	}
	if (err) {
		return err;
	}
	if (!first) {
		XAL_DEBUG("FAILED: the log is zeroed");
		return -ESTALE;
	}
	*cycle = first;
	if (first == last) {
		*head = 0;
		return 0;
	}
	if (last && last + 1 != first) {
		XAL_DEBUG("FAILED: log cycles first(%" PRIu32 ") last(%" PRIu32 ")", first, last);
		return -ESTALE;
	}

	/* The first block carrying 'last', as xlog_find_cycle_start() finds it */
	while (hi - lo > 1) {
		uint64_t mid = lo + (hi - lo) / 2;

		err = log_bb_cycle(xal, log_ofz, mid, &mid_cycle);
		if (err) {
			return err;
		}
		if (mid_cycle == last) {
			hi = mid;
		} else {
			lo = mid;
		}
	}
	*head = hi;

	return 0;
}

/* Blocks back from the head to look for the header of the last record: a record of the largest
 * log buffer, 256 KiB, and its header blocks */
#define LOG_SCAN_NBBS (256 * 1024 / XAL_ODF_LOG_BBSIZE + 8)

/**
 * Find the header of the last record before the head of a log of 'nbbs' blocks, the nearest block
 * that starts with the magic; the other blocks of a record start with their cycle. The log is read
 * backwards from the head in reads of up to BUF_NBYTES, or the MDTS when smaller.
 */
static int
log_last_record(struct xal *xal, uint64_t log_ofz, uint64_t nbbs, uint64_t head, uint64_t *rblk,
		uint8_t *header)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	const struct xnvme_geo *geo = xnvme_dev_get_geo(xal->dev);
	uint64_t max_nbytes = BUF_NBYTES < geo->mdts_nbytes ? BUF_NBYTES : geo->mdts_nbytes;
	uint64_t lba_nbbs = geo->lba_nbytes / XAL_ODF_LOG_BBSIZE;
	uint64_t chunk_nbbs = max_nbytes / geo->lba_nbytes * lba_nbbs;
	uint64_t pos = head, scanned = 0;
	int err;

	if (!lba_nbbs || !chunk_nbbs || nbbs % lba_nbbs) {
		return -EOPNOTSUPP;
	}
	while (scanned < LOG_SCAN_NBBS && scanned < nbbs) {
		uint64_t start, stop;

		if (!pos) {
			pos = nbbs;
		}
		stop = (pos + lba_nbbs - 1) / lba_nbbs * lba_nbbs;
		start = stop > chunk_nbbs ? stop - chunk_nbbs : 0;

		err = dev_read(xal->dev, be->buf, (stop - start) * XAL_ODF_LOG_BBSIZE,
			       log_ofz + start * XAL_ODF_LOG_BBSIZE);
		if (err) {
			XAL_DEBUG("FAILED: dev_read(); err(%d)", err);
			return err;
		}
		for (; pos > start && scanned < LOG_SCAN_NBBS; --pos, ++scanned) {
			const uint8_t *block =
			    (uint8_t *)be->buf + (pos - 1 - start) * XAL_ODF_LOG_BBSIZE;

			if (log_bb_be32(block, 0) == XAL_ODF_LOG_HEADER_MAGIC) {
				*rblk = pos - 1;
				memcpy(header, block, XAL_ODF_LOG_BBSIZE);
				return 0;
			}
		}
	}

	XAL_DEBUG("FAILED: no record header before the head(%" PRIu64 ")", head);

	return -ESTALE;
}

/**
 * Take from the internal log what tells its state apart: the LSN and CRC of the record at its
 * head, which must be the unmount record a clean unmount by the kernel leaves there
 *
 * The CRC covers the record's data, and with it the tid of its operation, which the kernel draws
 * at random; that is what tells two unmounts at the same LSN apart. A record that xfsprogs wrote,
 * when it formatted the log (mkfs, xfs_repair -L), has neither.
 *
 * @return On success, 0 is returned. -EUCLEAN is returned when the last record is not an unmount
 * record, as after a crash; -ENODATA when xfsprogs wrote it; -ESTALE when the log is not as a clean
 * unmount leaves it in another way; -EOPNOTSUPP for an external log; other negative errno on
 * error.
 */
static int
log_key(struct xal *xal, const struct xal_odf_sb *sb, struct index_key *key)
{
	uint64_t logstart = be64toh(sb->logstart);
	uint64_t nbbs = (uint64_t)be32toh(sb->logblocks) * xal->sb.blocksize / XAL_ODF_LOG_BBSIZE;
	uint8_t header[XAL_ODF_LOG_BBSIZE], data[XAL_ODF_LOG_BBSIZE];
	uint64_t log_ofz, head, rblk, hblks, end;
	uint32_t cycle, rcycle, version, h_size;
	int err;

	if (!logstart) {
		XAL_DEBUG("FAILED: external log");
		return -EOPNOTSUPP;
	}
	if (nbbs < 2) {
		return -ESTALE;
	}
	log_ofz = xal_fsbno_offset(xal, logstart);

	err = log_find_head(xal, log_ofz, nbbs, &head, &cycle);
	if (!err) {
		err = log_last_record(xal, log_ofz, nbbs, head, &rblk, header);
	}
	if (err) {
		return err;
	}

	version = log_bb_be32(header, XAL_ODF_LOG_H_VERSION);
	h_size = log_bb_be32(header, XAL_ODF_LOG_H_SIZE);
	hblks = 1;
	if ((version & XAL_ODF_LOG_VERSION_2) && h_size > XAL_ODF_LOG_HEADER_CYCLE_SIZE) {
		hblks =
		    (h_size + XAL_ODF_LOG_HEADER_CYCLE_SIZE - 1) / XAL_ODF_LOG_HEADER_CYCLE_SIZE;
	}
	end = (rblk + hblks +
	       (log_bb_be32(header, XAL_ODF_LOG_H_LEN) + XAL_ODF_LOG_BBSIZE - 1) /
		   XAL_ODF_LOG_BBSIZE) %
	      nbbs;
	/* A record that starts at or past the head wrapped around the end of the log, and was
	 * written in the cycle before the one block 0 carries */
	rcycle = head && rblk >= head ? cycle - 1 : cycle;

	memcpy(&key->log_lsn, header + XAL_ODF_LOG_H_LSN, sizeof(key->log_lsn));
	key->log_lsn = be64toh(key->log_lsn);
	memcpy(&key->log_crc, header + XAL_ODF_LOG_H_CRC, sizeof(key->log_crc));

	if (end != head || log_bb_be32(header, XAL_ODF_LOG_H_CYCLE) != rcycle ||
	    key->log_lsn != (((uint64_t)rcycle << 32) | rblk) ||
	    memcmp(header + XAL_ODF_LOG_H_FS_UUID, &sb->sb_uuid, sizeof(key->uuid))) {
		XAL_DEBUG("FAILED: the record at blk(%" PRIu64 ") does not end at head(%" PRIu64
			  ")",
			  rblk, head);
		return -ESTALE;
	}

	/* A clean unmount leaves one operation, flagged as an unmount, as xlog_check_unmount_rec()
	 * looks for it */
	if (log_bb_be32(header, XAL_ODF_LOG_H_NUM_LOGOPS) != 1) {
		XAL_DEBUG("FAILED: the log is dirty");
		return -EUCLEAN;
	}
	err = log_bb_read(xal, log_ofz, (rblk + hblks) % nbbs, data);
	if (err) {
		return err;
	}
	if (!(data[XAL_ODF_LOG_OH_FLAGS] & XAL_ODF_LOG_UNMOUNT_TRANS)) {
		XAL_DEBUG("FAILED: the log is dirty");
		return -EUCLEAN;
	}

	/* The tid, the first word of the record's data, is kept in h_cycle_data[0] */
	if (!key->log_crc || log_bb_be32(header, XAL_ODF_LOG_H_CYCLE_DATA) == LOG_XFSPROGS_TID) {
		XAL_DEBUG("FAILED: the unmount record is from xfsprogs");
		return -ENODATA;
	}

	return 0;
}

/* Keep the header sectors of the AG read for 'item' at its index in 'arg'; a batch_item_fn */
static int
ag_headers_store(struct xal *xal, const struct batch_item *item, const uint8_t *data, void *arg)
{
	memcpy((uint8_t *)arg + (size_t)item->idx * 4 * xal->sb.sectsize, data,
	       4 * xal->sb.sectsize);

	return 0;
}

/**
 * Read the key of the filesystem's current state: its UUID, the unmount record at the head of its
 * log, and the hash of the AG headers that xal_be_xfs_open() also takes
 */
static int
index_key_get(struct xal *xal, struct index_key *key)
{
	const struct xnvme_geo *geo = xnvme_dev_get_geo(xal->dev);
	size_t sect_nbytes = 4 * (size_t)xal->sb.sectsize;
	struct xal_odf_sb sb;
	struct batch_item *items;
	uint8_t *headers;
	int err;

	memset(key, 0, sizeof(*key));

	if (sect_nbytes % geo->lba_nbytes) {
		return -EOPNOTSUPP;
	}
	items = calloc(xal->sb.agcount, sizeof(*items));
	headers = malloc(xal->sb.agcount * sect_nbytes);
	if (!items || !headers) {
		XAL_DEBUG("FAILED: calloc()");
		err = -ENOMEM;
		goto exit;
	}
	for (uint32_t seqno = 0; seqno < xal->sb.agcount; ++seqno) {
		items[seqno] = (struct batch_item){
		    .ofz = xal_agbno_absolute_offset(xal, seqno, 0),
		    .nbytes = sect_nbytes,
		    .idx = seqno,
		};
	}
	err = batch_read(xal, items, xal->sb.agcount, ag_headers_store, headers);
	if (err) {
		XAL_DEBUG("FAILED: batch_read(AG headers); err(%d)", err);
		goto exit;
	}

	memcpy(&sb, headers, sizeof(sb));
	/* The key rests on what v5 does: lazy counters written at each unmount, sb_lsn */
	if ((be16toh(sb.versionnum) & XAL_ODF_SB_VERSION_NUMBITS) != XAL_ODF_SB_VERSION_5) {
		XAL_DEBUG("FAILED: not a v5 filesystem");
		err = -EOPNOTSUPP;
		goto exit;
	}
	memcpy(key->uuid, &sb.sb_uuid, sizeof(key->uuid));
	key->headers = fnv1a(FNV1A_BASIS, headers, xal->sb.agcount * sect_nbytes);

	err = log_key(xal, &sb, key);

exit:
	free(items);
	free(headers);

	return err;
}

static int
write_all(int fd, const void *buf, size_t nbytes)
{
	for (const uint8_t *cursor = buf; nbytes;) {
		ssize_t ret = write(fd, cursor, nbytes);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		cursor += ret;
		nbytes -= ret;
	}

	return 0;
}

static int
read_all(int fd, void *buf, size_t nbytes)
{
	for (uint8_t *cursor = buf; nbytes;) {
		ssize_t ret = read(fd, cursor, nbytes);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (!ret) {
			return -EPROTO;
		}
		cursor += ret;
		nbytes -= ret;
	}

	return 0;
}

/* Write 'hdr' and the pools to a new file next to 'path', sync it, and rename it over 'path' */
static int
index_file_write(struct xal *xal, const char *path, const struct index_file_hdr *hdr)
{
	char tmp[PATH_MAX], dir[PATH_MAX];
	const char *slash;
	int fd, err;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path) >= sizeof(tmp)) {
		return -ENAMETOOLONG;
	}
	slash = strrchr(path, '/');
	snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) + 1 : 1, slash ? path : ".");

	/* Mode 0600: the index names every file on the filesystem, whatever its permissions */
	fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0) {
		XAL_DEBUG("FAILED: mkostemp(%s); errno(%d)", tmp, errno);
		return -errno;
	}
	err = write_all(fd, hdr, sizeof(*hdr));
	if (!err) {
		err = write_all(fd, xal->inodes.memory, hdr->ninodes * hdr->inode_nbytes);
	}
	if (!err) {
		err = write_all(fd, xal->extents.memory, hdr->nextents * hdr->extent_nbytes);
	}
	if (!err && fsync(fd)) {
		err = -errno;
	}
	if (close(fd) && !err) {
		err = -errno;
	}
	if (!err && rename(tmp, path)) {
		err = -errno;
	}
	if (err) {
		XAL_DEBUG("FAILED: writing(%s); err(%d)", path, err);
		unlink(tmp);
		return err;
	}

	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	if (fsync(fd)) {
		err = -errno;
	}
	close(fd);

	return err;
}

int
xal_index_save(struct xal *xal, const char *path)
{
	struct xal_be_xfs *be;
	struct index_file_hdr hdr = {0};
	int err;

	if (!xal || !path) {
		return -EINVAL;
	}
	be = (struct xal_be_xfs *)&xal->be;
	if (be->base.type != XAL_BACKEND_XFS || xal->procrole == XAL_PROCROLE_SECONDARY) {
		return -EINVAL;
	}
	if (atomic_exchange(&xal->indexing, true)) {
		return -EBUSY;
	}
	if (xal->root_idx == XAL_POOL_IDX_NONE || xal_is_dirty(xal)) {
		XAL_DEBUG("FAILED: no index to save");
		err = -EINVAL;
		goto exit;
	}

	err = index_key_get(xal, &hdr.key);
	if (err) {
		XAL_DEBUG("FAILED: index_key_get(); err(%d)", err);
		goto exit;
	}
	/* The superblock is written when a mount of the filesystem is unmounted */
	if (hdr.key.headers != be->tree->headers) {
		XAL_DEBUG("FAILED: the filesystem changed since its inodes were read");
		err = -ESTALE;
		goto exit;
	}

	memcpy(hdr.magic, INDEX_FILE_MAGIC, sizeof(hdr.magic));
	hdr.version = INDEX_FILE_VERSION;
	hdr.shm_version = XAL_SHM_VERSION;
	hdr.hdr_nbytes = sizeof(hdr);
	hdr.inode_nbytes = sizeof(struct xal_inode);
	hdr.extent_nbytes = sizeof(struct xal_extent);
	hdr.ninodes = xal->inodes.free;
	hdr.nextents = xal->extents.free;
	hdr.root_idx = xal->root_idx;
	hdr.root_ino = be->tree->root_ino;
	hdr.sum =
	    pools_sum(pools_sum(FNV1A_BASIS, xal->inodes.memory, hdr.ninodes * hdr.inode_nbytes),
		      xal->extents.memory, hdr.nextents * hdr.extent_nbytes);
	if (be->tree->subtree) {
		strncpy(hdr.subtree, be->tree->subtree, XAL_PATH_MAXLEN);
	}

	err = index_file_write(xal, path, &hdr);

exit:
	atomic_store(&xal->indexing, false);

	return err;
}

/**
 * Whether the pools read by xal_index_load() form an index: every name terminated, every inode
 * a directory or a regular file, every entry of a directory after it and pointing back at it, and
 * every range inside the pools; the walks then end
 */
static bool
index_in_range(struct xal *xal, const struct index_file_hdr *hdr)
{
	/* xal_index() claims the root first, and a secondary takes it to be there */
	if (hdr->root_idx != 0 || hdr->root_idx >= hdr->ninodes ||
	    xal_inode_at(xal, hdr->root_idx)->parent_idx != XAL_POOL_IDX_NONE) {
		return false;
	}
	for (uint64_t i = 0; i < hdr->ninodes; ++i) {
		struct xal_inode *inode = xal_inode_at(xal, i);

		if (strnlen(inode->name, sizeof(inode->name)) != inode->namelen) {
			return false;
		}
		if (i != hdr->root_idx && inode->parent_idx >= i) {
			return false;
		}
		if (xal_inode_is_dir(inode)) {
			uint64_t first = inode->content.dentries.inodes_idx;
			uint64_t count = inode->content.dentries.count;

			if (count && (first <= i || first + count > hdr->ninodes)) {
				return false;
			}
			for (uint64_t k = first; k < first + count; ++k) {
				if (xal_inode_at(xal, k)->parent_idx != i) {
					return false;
				}
			}
		} else if (xal_inode_is_file(inode)) {
			if ((uint64_t)inode->content.extents.extent_idx +
				inode->content.extents.count >
			    hdr->nextents) {
				return false;
			}
		} else {
			return false;
		}
	}

	return true;
}

/* Read the index file at 'path' into the pools; see xal_index_load() */
static int
index_load(struct xal *xal, const char *path)
{
	struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
	struct index_file_hdr hdr;
	struct index_key key;
	struct stat st;
	uint32_t idx;
	int fd, err;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		XAL_DEBUG("FAILED: open(%s); errno(%d)", path, errno);
		return -errno;
	}
	if (fstat(fd, &st)) {
		err = -errno;
		goto exit;
	}
	/* What the file says is what fil reads for each path */
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
		XAL_DEBUG("FAILED: %s is not a file only this user can write", path);
		err = -EPERM;
		goto exit;
	}
	err = read_all(fd, &hdr, sizeof(hdr));
	if (err) {
		goto exit;
	}
	if (memcmp(hdr.magic, INDEX_FILE_MAGIC, sizeof(hdr.magic)) ||
	    hdr.version != INDEX_FILE_VERSION || hdr.shm_version != XAL_SHM_VERSION ||
	    hdr.hdr_nbytes != sizeof(hdr) || hdr.inode_nbytes != sizeof(struct xal_inode) ||
	    hdr.extent_nbytes != sizeof(struct xal_extent) || hdr.ninodes > UINT32_MAX ||
	    hdr.nextents > UINT32_MAX ||
	    (uint64_t)st.st_size !=
		sizeof(hdr) + hdr.ninodes * hdr.inode_nbytes + hdr.nextents * hdr.extent_nbytes) {
		XAL_DEBUG("FAILED: %s is not an index this build reads", path);
		err = -EPROTO;
		goto exit;
	}
	hdr.subtree[XAL_PATH_MAXLEN] = '\0';
	if (strcmp(hdr.subtree, be->tree->subtree ? be->tree->subtree : "")) {
		XAL_DEBUG("FAILED: %s is of subtree(%s)", path, hdr.subtree);
		err = -ESTALE;
		goto exit;
	}

	err = index_key_get(xal, &key);
	if (err) {
		XAL_DEBUG("FAILED: index_key_get(); err(%d)", err);
		goto exit;
	}
	if (memcmp(&key, &hdr.key, sizeof(key))) {
		XAL_DEBUG("INFO: the filesystem changed since %s was saved", path);
		err = -ESTALE;
		goto exit;
	}
	/* The AGs the handle keeps (inobt roots, counts) are those xal_open() read */
	if (key.headers != be->tree->headers) {
		XAL_DEBUG("FAILED: the filesystem changed since it was opened");
		err = -ESTALE;
		goto exit;
	}

	atomic_store(xal->index_state, XAL_STATE_INDEXING);
	atomic_fetch_add(xal->seq_lock, 1);

	xal->root_idx = XAL_POOL_IDX_NONE;
	xal_pool_clear(&xal->inodes);
	xal_pool_clear(&xal->extents);

	if (hdr.ninodes) {
		err = xal_pool_claim_inodes(&xal->inodes, hdr.ninodes, &idx);
	}
	if (!err) {
		err = read_all(fd, xal->inodes.memory, hdr.ninodes * hdr.inode_nbytes);
	}
	if (!err && hdr.nextents) {
		err = xal_pool_claim_extents(&xal->extents, hdr.nextents, &idx);
	}
	if (!err) {
		err = read_all(fd, xal->extents.memory, hdr.nextents * hdr.extent_nbytes);
	}
	if (!err &&
	    (pools_sum(pools_sum(FNV1A_BASIS, xal->inodes.memory, hdr.ninodes * hdr.inode_nbytes),
		       xal->extents.memory, hdr.nextents * hdr.extent_nbytes) != hdr.sum ||
	     !index_in_range(xal, &hdr))) {
		XAL_DEBUG("FAILED: %s is damaged", path);
		err = -EPROTO;
	}
	if (!err) {
		xal->root_idx = hdr.root_idx;
		be->tree->root_ino = hdr.root_ino;

		/* The inodes are not those of this index; xal_index() needs them read again */
		dirblocks_free(xal);
		dinodes_free(xal);
	} else {
		xal_pool_clear(&xal->inodes);
		xal_pool_clear(&xal->extents);
	}

	atomic_fetch_add(xal->seq_lock, 1);
	xal_mark_index_done(xal, err);
	atomic_store(&xal->last_index_err, err);

exit:
	close(fd);

	return err;
}

int
xal_index_load(struct xal *xal, const char *path)
{
	struct xal_be_xfs *be;
	int err;

	if (!xal || !path) {
		return -EINVAL;
	}
	be = (struct xal_be_xfs *)&xal->be;
	if (be->base.type != XAL_BACKEND_XFS || xal->procrole == XAL_PROCROLE_SECONDARY) {
		return -EINVAL;
	}
	if (atomic_exchange(&xal->indexing, true)) {
		return -EBUSY;
	}

	err = index_load(xal, path);

	atomic_store(&xal->indexing, false);

	return err;
}
