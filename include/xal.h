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

struct xal_shared_state {
	enum xal_backend type;
	struct xal_sb sb;
	char mountpoint[XAL_PATH_MAXLEN];
	char subtree[XAL_PATH_MAXLEN]; ///< Empty when the index covers the whole mount
	atomic_int index_state; ///< One of enum xal_state
	atomic_int seq_lock; ///< Even when stable; odd while the pools are being rewritten in place
};

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

	/* Held for the duration of xal_index(); a second caller is refused with -EBUSY. Separate
	 * from index_state because xal_mark_dirty() clears XAL_STATE_INDEXING unconditionally, so
	 * that state cannot also be the thing that excludes an index. Not shared: a secondary
	 * cannot index. */
	atomic_bool indexing;

	/* Result of the last completed xal_index(). The watch loop cannot infer it -- a failed
	 * index and a successful one with a mark landing mid-rebuild both leave DIRTY with
	 * seq_lock advanced, and want opposite treatment. Not set by the -EBUSY path. */
	atomic_int last_index_err;
};

int
search_by_traversal(struct xal *xal, struct xal_inode *root, char *path, char *basepath, struct xal_inode **inode);

void
xal_mark_index_done(struct xal *xal, int err);

#endif /* XAL_H */
