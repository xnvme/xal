#define _GNU_SOURCE
#include <endian.h>
#include <inttypes.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_inotify.h>
#include <xal_be_xfs.h>
#include <xal_odf.h>

#define BMAP_BLOCK_SIZE	512

static int
xal_be_xfs_pp(struct xal *xal, struct xal_be_xfs *be);

static int
xal_be_fiemap_pp(struct xal_be_fiemap *be);

int
xal_ag_pp(struct xal_ag *ag)
{
	int wrtn = 0;

	if (!ag) {
		wrtn += printf("xal_ag: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ag:\n");
	wrtn += printf("  seqno: %" PRIu32 "\n", ag->seqno);
	wrtn += printf("  offset: %" PRIiMAX "\n", (intmax_t)ag->offset);
	wrtn += printf("  agf_length: %" PRIu32 "\n", ag->agf_length);
	wrtn += printf("  agi_count: %" PRIu32 "\n", ag->agi_count);
	wrtn += printf("  agi_root: %" PRIu32 "\n", ag->agi_root);
	wrtn += printf("  agi_level: %" PRIu32 "\n", ag->agi_level);

	return wrtn;
}

int
xal_pp(struct xal *xal)
{
	struct xal_backend_base *be;
	int wrtn = 0;

	if (!xal) {
		wrtn += printf("xal: ~\n");
		return wrtn;
	}

	be = (struct xal_backend_base *)&xal->be;

	wrtn += printf("xal:\n");
	wrtn += printf("  sb.blocksize: %" PRIu32 "\n", xal->sb.blocksize);
	wrtn += printf("  sb.sectsize: %" PRIu16 "\n", xal->sb.sectsize);
	wrtn += printf("  sb.inodesize: %" PRIu16 "\n", xal->sb.inodesize);
	wrtn += printf("  sb.inopblock: %" PRIu16 "\n", xal->sb.inopblock);
	wrtn += printf("  sb.inopblog: %" PRIu8 "\n", xal->sb.inopblog);
	wrtn += printf("  sb.icount: %" PRIu64 "\n", xal->sb.icount);
	wrtn += printf("  sb.nallocated: %" PRIu64 "\n", xal->sb.nallocated);
	wrtn += printf("  sb.rootino: %" PRIu64 "\n", xal->sb.rootino);
	wrtn += printf("  sb.agblocks: %" PRIu32 "\n", xal->sb.agblocks);
	wrtn += printf("  sb.agblklog: %" PRIu8 "\n", xal->sb.agblklog);
	wrtn += printf("  sb.agcount: %" PRIu32 "\n", xal->sb.agcount);
	wrtn += printf("  sb.dirblocksize: %" PRIu32 "\n", xal->sb.dirblocksize);

	switch (be->type) {
		case XAL_BACKEND_XFS:
			struct xal_be_xfs *xfs = (struct xal_be_xfs *)be;
			wrtn += xal_be_xfs_pp(xal, xfs);
			break;
		case XAL_BACKEND_FIEMAP:
			struct xal_be_fiemap *fiemap = (struct xal_be_fiemap *)be;
			wrtn += xal_be_fiemap_pp(fiemap);
			break;
	}

	return wrtn;
}

int
xal_odf_sb_pp(void *buf)
{
	struct xal_odf_sb *sb = buf;
	int wrtn = 0;

	wrtn += printf("xal_odf_sb:\n");
	wrtn += printf("  magicnum: 0x%" PRIx32 "\n", be32toh(sb->magicnum));
	wrtn += printf("  blocksize: 0x%" PRIx32 "x\n", be32toh(sb->blocksize));
	wrtn += printf("  rootino: %" PRIx64 "zu\n", be64toh(sb->rootino));
	wrtn += printf("  agblocks: %" PRIx32 "d\n", be32toh(sb->agblocks));
	wrtn += printf("  agcount: %" PRIx32 "d\n", be32toh(sb->agcount));
	wrtn += printf("  sectsize: %" PRIx16 "u\n", be16toh(sb->sectsize));
	wrtn += printf("  inodesize: %" PRIx16 "u\n", be16toh(sb->inodesize));
	wrtn += printf("  fname: '%.*s'\n", XAL_ODF_LABEL_MAX, sb->fname);
	wrtn += printf("  dirblklog: 0x%" PRIu8 "x\n", sb->dirblklog);

	return wrtn;
}

int
xal_odf_agf_pp(void *buf)
{
	struct xal_odf_agf *agf = buf;
	int wrtn = 0;

	wrtn += printf("xal_odf_agf:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agf->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agf->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agf->length));

	return wrtn;
}

int
xal_odf_agi_pp(void *buf)
{
	struct xal_odf_agi *agi = buf;
	int wrtn = 0;

	wrtn += printf("xal_agi:\n");
	wrtn += printf("  magicnum: 0x%x\n", be32toh(agi->magicnum));
	wrtn += printf("  seqno: 0x%x\n", be32toh(agi->seqno));
	wrtn += printf("  length: 0x%x\n", be32toh(agi->length));

	return wrtn;
}

int
xal_odf_agfl_pp(void *buf)
{
	struct xal_odf_agfl *agfl = buf;
	int wrtn = 0;

	wrtn += printf("xal_odf_agfl:\n");
	wrtn += printf("  magicnum: 0x%x\n", agfl->magicnum);
	wrtn += printf("  seqno: 0x%x\n", agfl->seqno);

	return wrtn;
}

const char *
xal_odf_dinode_format_str(int val)
{
	switch (val) {
	case XAL_DINODE_FMT_BTREE:
		return "btree";
	case XAL_DINODE_FMT_DEV:
		return "dev";
	case XAL_DINODE_FMT_EXTENTS:
		return "extents";
	case XAL_DINODE_FMT_LOCAL:
		return "local";
	case XAL_DINODE_FMT_UUID:
		return "uuid";
	};

	return "INODE_FORMAT_UNKNOWN";
}

int
xal_inode_pp(struct xal *xal, struct xal_inode *inode)
{
	int wrtn = 0;

	if (!inode) {
		wrtn += printf("xal_inode: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_inode:\n");
	wrtn += printf("  ino: 0x%08" PRIX64 "\n", inode->ino);
	wrtn += printf("  size: %" PRIu64 "\n", inode->size);
	wrtn += printf("  namelen: %" PRIu8 "\n", inode->namelen);
	wrtn += printf("  name: '%.256s'\n", inode->name);
	wrtn += printf("  ftype: %" PRIu8 "\n", inode->ftype);

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR:
		wrtn += printf("  dentries.count: %u\n", inode->content.dentries.count);

		for (uint8_t i = 0; i < inode->content.dentries.count; ++i) {
			struct xal_inode *child = xal_inode_at(xal, inode->content.dentries.inodes_idx + i);

			if (!child) {
				wrtn += printf("  dentries: ~ # index out of range\n");
				break;
			}
			xal_inode_pp(xal, child);
		}

		break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		uint32_t blocksize = xal_get_sb_blocksize(xal);
		wrtn += printf("  extents.count: %u\n", inode->content.extents.count);
		for (uint32_t i = 0; i < inode->content.extents.count; ++i) {
			struct xal_extent *extent = xal_extent_at(xal, inode->content.extents.extent_idx + i);
	        size_t fofz_begin, fofz_end, bofz_begin, bofz_end;

			if (!extent) {
				wrtn += printf("- ~ # index out of range\n");
				break;
			}

	        fofz_begin = (extent->start_offset * blocksize) / BMAP_BLOCK_SIZE;
	        fofz_end = fofz_begin + (extent->nblocks * blocksize) / BMAP_BLOCK_SIZE - 1;
	        bofz_begin = xal_fsbno_offset(xal, extent->start_block) / BMAP_BLOCK_SIZE;
	        bofz_end = bofz_begin + (extent->nblocks * blocksize) / BMAP_BLOCK_SIZE - 1;
			wrtn += printf("- [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %"
				    PRIu64 "]\n", fofz_begin, fofz_end, bofz_begin, bofz_end);
		}
		break;
	}

	fflush(stdout);
	return wrtn;
}

const char *
mode_to_type_str(uint32_t mode)
{
	if (S_ISDIR(mode)) {
		return "directory";
	}
	if (S_ISREG(mode)) {
		return "file";
	}

	return "UNEXPECTED";
}

int
xal_odf_dinode_pp(void *buf)
{
	struct xal_odf_dinode *dinode = buf;
	int wrtn = 0;

	wrtn += printf("xal_dinode:\n");
	wrtn += printf("  magic: 0x%x | 0x%x\n", be16toh(dinode->di_magic), XAL_DINODE_MAGIC);
	wrtn += printf("  mode: 0x%" PRIx16 " | '%s'\n", be16toh(dinode->di_mode),
		       mode_to_type_str(be16toh(dinode->di_mode)));
	wrtn += printf("  format: 0x%" PRIu8 " | '%s'\n", dinode->di_format,
		       xal_odf_dinode_format_str(dinode->di_format));

	wrtn += printf("  ino: %" PRIu64 "\n", be64toh(dinode->ino));

	return wrtn;
}

int
xal_extent_pp(struct xal_extent *extent)
{
	int wrtn = 0;

	if (!extent) {
		wrtn += printf("xal_extent: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_extent:\n");
	wrtn += printf("  start_offset: %" PRIu64 "\n", extent->start_offset);
	wrtn += printf("  start_block: %" PRIu64 "\n", extent->start_block);
	wrtn += printf("  nblocks: %" PRIu64 "\n", extent->nblocks);
	wrtn += printf("  flag: %" PRIu8 "\n", extent->flag);

	return wrtn;
}

int
xal_extent_converted_pp(struct xal_extent_converted *extent)
{
	int wrtn = 0;

	if (!extent) {
		wrtn += printf("xal_extent_converted: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_extent_converted:\n");
	wrtn += printf("  start_offset: %" PRIu64 "\n", extent->start_offset);
	wrtn += printf("  start_block: %" PRIu64 "\n", extent->start_block);
	
	switch (extent->unit) {
		case XAL_EXTENT_UNIT_BYTES:
			wrtn += printf("  bytes: %" PRIu64 "\n", extent->size);
			wrtn += printf("  unit: XAL_EXTENT_UNIT_BYTES\n");
		break;
		
		case XAL_EXTENT_UNIT_LBA:
			wrtn += printf("  nblocks: %" PRIu64 "\n", extent->size);
			wrtn += printf("  unit: XAL_EXTENT_UNIT_LBA\n");
			break;
	}

	return wrtn;
}

int
xal_odf_btree_iab3_sfmt_pp(struct xal_odf_btree_sfmt *iab3)
{
	int wrtn = 0;

	if (!iab3) {
		wrtn += printf("xal_ofd_btree_iab3: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ofd_btree_iab3:\n");
	wrtn += printf("  magic: 0x%08" PRIX32 " / '%.4s'\n", iab3->magic.num, iab3->magic.text);
	wrtn += printf("  level: %" PRIu16 "\n", iab3->pos.level);
	wrtn += printf("  numrecs: %" PRIu16 "\n", iab3->pos.numrecs);
	wrtn += printf("  leftsib: 0x%08" PRIX32 "\n", iab3->siblings.left);
	wrtn += printf("  rightsib: 0x%08" PRIX32 "\n", iab3->siblings.right);

	wrtn += printf("  blkno: %" PRIu64 "\n", iab3->blkno / 8);

	return wrtn;
}

int
xal_odf_inobt_rec_pp(struct xal_odf_inobt_rec *rec)
{
	int wrtn = 0;

	if (!rec) {
		wrtn += printf("xal_ofd_inobt_rec: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_ofd_inobt_rec:\n");
	wrtn += printf("  startino: %" PRIu32 "\n", rec->startino);
	wrtn += printf("  holemask: %" PRIu16 "\n", rec->holemask);
	wrtn += printf("  count: %" PRIu8 "\n", rec->count);
	wrtn += printf("  freecount: %" PRIu8 "\n", rec->freecount);
	wrtn += printf("  free: %" PRIu64 "\n", rec->free);

	return wrtn;
}

static int
xal_be_xfs_pp(struct xal *xal, struct xal_be_xfs *be)
{
	int wrtn = 0;

	if (!be) {
		wrtn += printf("xal_be_xfs: ~\n");
		return wrtn;
	}

	for (uint32_t i = 0; i < xal->sb.agcount; ++i) {
		wrtn += xal_ag_pp(&be->ags[i]);
	}

	return wrtn;
}

static int
xal_be_fiemap_pp(struct xal_be_fiemap *be)
{
	int wrtn = 0;

	if (!be) {
		wrtn += printf("xal_be_fiemap: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_be_fiemap:\n");
	wrtn += printf("  mountpoint: %s\n", be->mountpoint);
	wrtn += printf("  subtree: %s\n", be->subtree ? be->subtree : "~");

	return wrtn;
}

int
xal_inotify_pp(struct xal_inotify *inotify)
{
	int wrtn = 0;

	if (!inotify) {
		wrtn += printf("xal_inotify: ~\n");
		return wrtn;
	}

	wrtn += printf("xal_inotify:\n");
	wrtn += printf("  fd: %d\n", inotify->fd);
	wrtn += printf("  inode_map addr: %p\n", inotify->inode_map);

	switch (inotify->watch_mode) {
		case XAL_WATCHMODE_NONE:
			wrtn += printf("  watchmode: XAL_WATCHMODE_NONE\n");
			break;

		case XAL_WATCHMODE_REFLINK_SNAPSHOT:
			wrtn += printf("  watchmode: XAL_WATCHMODE_REFLINK_SNAPSHOT\n");
			break;

		default:
			wrtn += printf("  watchmode: ?\n");
			break;
	}

	wrtn += printf("  watch_thread_id: %ld\n", inotify->watch_thread_id);

	return wrtn;
}
