#ifndef XAL_H
#define XAL_H

#include <stdatomic.h>
#include <unistd.h>
#include <xal_pool.h>

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode
#define XAL_BACKEND_SIZE 72

struct xal_backend_base {
	enum xal_backend type;
	int (*index)(struct xal *xal);
	void (*close)(struct xal *xal);
};

enum xal_procrole {
	XAL_PROCROLE_SINGLE = 0,
	XAL_PROCROLE_PRIMARY = 1,
	XAL_PROCROLE_SECONDARY = 2,
};

enum xal_state {
	XAL_STATE_CLEAN = 0,	///< The representation matches the last indexed filesystem state
	XAL_STATE_DIRTY = 1,	///< A breaking change occurred that no index has begun to observe
	XAL_STATE_INDEXING = 2, ///< xal_index() is rebuilding the representation
};

#define XAL_SHM_MAGIC 0x58414c5341ULL ///< "XALSA", XAL Shared ABI; zero until published

/**
 * Bumped whenever a secondary built at one version would misread a region published at another.
 * That covers the layout of this struct and of the pool elements, and equally what the pool
 * contents mean: changing xal_inode.name from an assembled path to a leaf name leaves every size
 * and offset intact and still requires a bump.
 */
#define XAL_SHM_VERSION 2

struct xal_shared_state {
	_Atomic uint64_t magic; ///< XAL_SHM_MAGIC, stored last, so zero means not yet published
	uint32_t version; ///< XAL_SHM_VERSION
	enum xal_backend type;
	struct xal_sb sb;
	char mountpoint[XAL_PATH_MAXLEN];
	char subtree[XAL_PATH_MAXLEN]; ///< Empty when the index covers the whole mount
	atomic_int index_state; ///< One of enum xal_state
	atomic_int seq_lock; ///< Even when stable; odd while the pools are being rewritten in place
};
XAL_STATIC_ASSERT(offsetof(struct xal_shared_state, magic) == 0, "shm ABI");
XAL_STATIC_ASSERT(offsetof(struct xal_shared_state, version) == 8, "shm ABI");

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 *
 * @struct xal
 */
struct xal {
	struct xnvme_dev *dev;
	struct xal_pool inodes;  ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	uint32_t root_idx;       ///< Index of the root inode in the inodes pool
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
	atomic_int *index_state; ///< One of enum xal_state; may point to external shared memory
	atomic_int _index_state_storage; ///< Backing store for index_state when shm_name is not set
	atomic_int *seq_lock;    ///< An uneven number indicates the struct is being modified and is not safe to read; may point to external shared memory
	atomic_int _seq_lock_storage; ///< Backing store for seq_lock when shm_name is not set
	struct xal_shared_state *state; ///< Mapped shared state region; non-NULL when shm_name was set
	char *state_shm_name;           ///< Name of the _state shm region; set by primary only, for unlink on close
	enum xal_procrole procrole;
};

int
search_by_traversal(struct xal *xal, struct xal_inode *root, char *path, char *basepath, struct xal_inode **inode);

void
xal_mark_index_done(struct xal *xal, int err);

#endif /* XAL_H */
