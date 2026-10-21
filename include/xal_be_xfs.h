#ifndef XAL_BE_XFS_H
#define XAL_BE_XFS_H

/**
 * XAL Allocation Group
 *
 * Contains a subset of allocation group fields, individual data for allocation
 * groups in host-endian
 *
 * Byte-order: host-endianess
 */
struct xal_ag {
	uint32_t seqno;
	off_t offset;	     ///< Offset on disk in bytes; seqno * agblocks * blocksize
	uint32_t agf_length; ///< Size of allocation group, in blocks
	uint32_t agi_count;  ///< Number of allocated inodes, counting from 1
	uint32_t agi_root;   ///< Block number positioned relative to the AG
	uint32_t agi_level;  ///< levels in inode btree
};

/**
 * What the index of the XFS backend covers
 */
struct xal_be_xfs_tree {
	char *subtree;	   ///< opts.subtree, normalized; NULL for the whole filesystem
	uint64_t root_ino; ///< Inode the index is rooted at: sb.rootino or the subtree's
	uint64_t headers;  ///< Hash of the AG headers as xal_open() read them, which
			   ///< xal_index_save() and xal_index_load() require them still to be
};

struct xal_be_xfs {
	struct xal_backend_base base;
	void *buf;            ///< A single buffer for repetitive IO
	uint8_t *dinodes;     ///< Array of inodes in on-disk-format
	void *dinodes_map;    ///< How an inode is found in dinodes; see struct dinodes_index
	struct xal_ag *ags;   ///< Array of 'agcount' number of allocation-groups
	void *dirblocks;      ///< Directory blocks read ahead for xal_index(); NULL outside it
	struct xal_be_xfs_tree *tree; ///< Set up by xal_be_xfs_open() and xal_from_shm()

	uint8_t _rsvd[8];
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_xfs) == XAL_BACKEND_SIZE, "Incorrect size");

void
xal_be_xfs_close(struct xal *xal);

int
xal_be_xfs_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts);

#endif /* XAL_BE_XFS_H */
