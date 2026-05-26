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

struct xal_be_xfs {
	struct xal_backend_base base;
	void *buf;            ///< A single buffer for repetitive IO
	uint8_t *dinodes;     ///< Array of inodes in on-disk-format
	void *dinodes_map;    ///< Map of dinodes for O(1) ~ avg. lookup
	struct xal_ag *ags;   ///< Array of 'agcount' number of allocation-groups

	uint8_t _rsvd[8];
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_xfs) == XAL_BACKEND_SIZE, "Incorrect size");

void
xal_be_xfs_close(struct xal *xal);

int
xal_be_xfs_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts);
