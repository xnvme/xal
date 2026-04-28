#ifndef LIBXAL_H
#define LIBXAL_H

/**
 * Public API for the XAL library
 *
 * On-disk Storage Format
 * ======================
 *
 * The on-disk storage format is not exposed in these headers since the intent is that the user
 * should not need to rely on them. Rather, they are handled by the library-implementation and
 * values communicated via pretty-printers and transformed into "simplified" representations.
 *
 * Endianness
 * ----------
 *
 * The XFS on-disk format make use of integer values in big-endian format, thus value convertion
 * to little-endian is needed on e.g. x86 systems. Such helpers are not exposed here, rather the
 * conversions are performed by the library and the representations found here are in "native"
 * format.
 *
 * Files and Directories
 * ---------------------
 *
 * XFS has 4 or 5 ways of storing directory indexes (filename + inode number) optimized by
 * use-case. When XAL parses these from the on-disk-format, then they normalized into the
 * form represented by 'struct xal_inode'. All inodes are backed by a overcommitted memory-map
 * represented by 'struct xal_pool'.
 *
 */
#include <inttypes.h>
#include <libxal_util.h>
#include <libxnvme.h>
#include <stddef.h>
#include <sys/types.h>

#define XAL_INODE_NAME_MAXLEN 255
#define XAL_PATH_MAXLEN 255
#define XAL_POOL_IDX_NONE UINT32_MAX

enum xal_backend {
	XAL_BACKEND_XFS     = 1,
	XAL_BACKEND_FIEMAP  = 2,
};

enum xal_watchmode {
	XAL_WATCHMODE_NONE             = 0,  ///< There will be no notifications of changes to the filesystem.
	XAL_WATCHMODE_DIRTY_DETECTION  = 1,  ///< When changes to the file system occurs, the xal struct will become "dirty" indicating that the representation of the file system is stale.
	XAL_WATCHMODE_EXTENT_UPDATE    = 2,  ///< When other changes to the file system occurs, the xal struct will be automatically updated if the extent information is the only subject to change, otherwise the xal struct will become "dirty" indicating that the representation of the file system is stale.
};

enum xal_file_lookupmode {
	XAL_FILE_LOOKUPMODE_TRAVERSE = 0,  ///< Traverses the file tree from xal->root using binary search at each level to find the inode.
	XAL_FILE_LOOKUPMODE_HASHMAP = 1,   ///< Uses a hash map for constant-time inode lookup in xal_get_inode(), with higher memory usage.
};

struct xal_opts {
	enum xal_backend be;
	enum xal_watchmode watch_mode;
	enum xal_file_lookupmode file_lookupmode;
	const char *shm_name; ///< If set, pool memory is backed by POSIX shared memory with this base name, see @xal_from_pools() for sharing the pools across processes
};

struct xal_extent {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	uint8_t flag;
} __attribute__((packed));

int
xal_extent_pp(struct xal_extent *extent);

enum xal_extent_unit {
	XAL_EXTENT_UNIT_BYTES = 0,
	XAL_EXTENT_UNIT_LBA = 1,
};

struct xal_extent_converted {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t size;
	enum xal_extent_unit unit;
};

int
xal_extent_converted_pp(struct xal_extent_converted *extent);

struct xal_inode;

struct xal_dentries {
	uint32_t inodes_idx; ///< Index of first child in xal->inodes pool
	uint32_t count;      ///< Number of children; for directories
};

struct xal_extents {
	uint32_t extent_idx; ///< Index of first extent in xal->extents pool
	uint32_t count;      ///< Number of extents
};

union xal_inode_content {
	struct xal_dentries dentries;
	struct xal_extents extents;
};

struct xal_inode {
	uint64_t ino;  ///< Inode number of the directory entry; Should the AG be added here?
	uint64_t size; ///< Size in bytes
	union xal_inode_content content;
	uint8_t ftype;			      ///< File-type (directory, filename, symlink etc.)
	uint8_t namelen;		      ///< Length of the name; not counting nul-termination
	char name[XAL_INODE_NAME_MAXLEN + 1]; ///< Name; not including nul-termination
	uint8_t reserved[22];
	uint32_t parent_idx;
};

/**
 * XAL
 *
 * Opaque struct.
 *
 * @see xal_open()
 *
 * @struct xal
 */
struct xal;

struct xal_sb {
	uint32_t blocksize;    ///< Size of a block, in bytes
	uint16_t sectsize;     ///< Size of a sector, in bytes
	uint16_t inodesize;    ///< inode size, in bytes
	uint16_t inopblock;    ///< inodes per block
	uint8_t inopblog;      ///< log2 of inopblock
	uint64_t icount;       ///< allocated inodes
	uint64_t nallocated;   ///< Allocated inodes - sum of agi_count
	uint64_t rootino;      ///< root inode number, in global-address format
	uint32_t agblocks;     ///< Size of an allocation group, in blocks
	uint8_t agblklog;      ///< log2 of 'agblocks' (rounded up)
	uint32_t agcount;      ///< Number of allocation groups
	uint32_t dirblocksize; ///< Size of a directory block, in bytes
	uint32_t lba_blksze;   ///< LBA block size
};

struct xal_inode *
xal_inode_at(struct xal *xal, uint32_t idx);

struct xal_extent *
xal_extent_at(struct xal *xal, uint32_t idx);

uint32_t
xal_inode_idx(struct xal *xal, struct xal_inode *inode);

int
xal_inode_pp(struct xal *xal, struct xal_inode *inode);

/**
 * Returns the extent information in bytes.
 *
 * @param xal The xal struct obtained when opened with xal_open()
 * @param extent A pointer to a `xal_extent`
 * @param output A pointer to the output of the conversion
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_extent_in_bytes(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output);

/**
 * Returns the extent information in blocks using the LBA format on the block device.
 *
 * @param xal The xal struct obtained when opened with xal_open()
 * @param extent A pointer to a `xal_extent`
 * @param output A pointer to the output of the conversion
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_extent_in_lba(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output);

/**
 * Returns the root of the file-system
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * 
 * @return On success, the inode at the root of the file-system is retuned
 */
struct xal_inode *
xal_get_root(struct xal *xal);

/**
 * Returns true if breaking changes to the mounted file-system have been found, which
 * invalidates the representation of the file-system in the xal->root field.
 * 
 * @note If the xal struct was not opened with backend "fiemap", this will always
 * return false.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * 
 * @return a boolean value, indicating whether breaking changes to the file-system have
 *         been found.
 */
bool
xal_is_dirty(struct xal *xal);

/**
 * Returns the current value of the sequence lock.
 * 
 * An uneven number indicates the struct is being modified and is not safe to read. An even number
 * indicates that the struct is safe to read.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * 
 * @return the current value of the sequence lock
 */
int
xal_get_seq_lock(struct xal *xal);

const struct xal_sb *
xal_get_sb(struct xal *xal);

uint32_t
xal_get_sb_blocksize(struct xal *xal);

typedef int (*xal_walk_cb)(struct xal *xal, struct xal_inode *inode, void *cb_args, int level);

int
xal_pp(struct xal *xal);

/**
 * Open and decode the file-system meta-data on the given device
 *
 * This will retrieve the Superblock (sb) and Allocation Group (AG) headers for all AGs. These are
 * utilized to instantiate the 'struct xal' with a subset of the on-disk-format parsed to native
 * format.
 *
 * @param dev Pointer to xnvme device handled as retrieved with xnvme_dev_open()
 * @param xal Pointer
 * @param opts Pointer to options, see xal_opts
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts);

void
xal_close(struct xal *xal);

/**
 * Construct a read-only xal from externally provided pool memory.
 *
 * Intended for processes that receive pool memory via POSIX shared memory from elsewhere.
 * The caller is responsible for mapping the shared memory regions before calling this with the
 * shm_name given in the xal_opts.
 * The resulting xal can be closed with xal_close(), which will free the struct xal allocation but
 * will not munmap or unlink the pool memory regions — that remains the caller's responsibility.
 *
 * @param sb           Superblock metadata
 * @param mountpoint   Mountpoint of the file system
 * @param inodes_mem   Pointer to the mapped inode pool memory; the root inode must be at index 0
 * @param extents_mem  Pointer to the mapped extent pool memory
 * @param out          Output pointer for the constructed xal
 *
 * @return On success, 0. On error, negative errno.
 */
int
xal_from_pools(const struct xal_sb *sb, const char *mountpoint, void *inodes_mem,
	void *extents_mem, struct xal **out);

/**
 * Retrieve inodes from disk and decode the on-disk-format of the retrieved data
 *
 * @param xal Pointer to the xal
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_dinodes_retrieve(struct xal *xal);

/**
 * Produce an index of the directory and files stored on the device
 *
 * Assumes that you have retrieved all the inodes from disk via xal_dinodes_retrieve() if opened with
 * backend XAL_BACKEND_XFS.
 * 
 * When called, any index created from previous calls to xal_index() are cleared.
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_index(struct xal *xal);

/**
 * Callback invoked by the background watch thread immediately after the xal struct is marked
 * dirty. Dirty means breaking filesystem changes (file creation, deletion, or rename) were
 * detected, and the in-memory representation is now stale.
 *
 * The callback is called from the watch thread; keep it short and thread-safe.
 *
 * @param xal     The xal struct that became dirty.
 * @param cb_args The opaque pointer passed to xal_watch_filesystem().
 */
typedef void (*xal_dirty_cb)(struct xal *xal, void *cb_args);

/**
 * Start a background thread listening to inotify events of changes to the file system on the
 * block device.
 *
 * Assumes that
 *  - the file system is mounted,
 *  - you have run xal_open() with backend FIEMAP and a watch_mode other than XAL_WATCHMODE_NONE,
 *  - you have indexed the file system with xal_index().
 *
 * If these assumptions do not hold, this will result in an error.
 *
 * @param xal     The xal struct obtained when opened with xal_open().
 * @param cb      Optional callback invoked when xal becomes dirty. May be NULL.
 * @param cb_args Opaque pointer forwarded to cb. May be NULL.
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_watch_filesystem(struct xal *xal, xal_dirty_cb cb, void *cb_args);

/**
 * Stop the background thread listening to inotify events of changes to the file system on the
 * block device.
 * 
 * Assumes that
 *  - you have run xal_open() with backend FIEMAP and a watch_mode other than XAL_WATCHMODE_NONE,
 *  - the background thread is running, see`xal_watch_filesystem()`.
 * 
 * If these assumptions do not hold, this will result in an error.
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_stop_watching_filesystem(struct xal *xal);

/**
 * Recursively walk the given directory
 *
 * Invoking the given cb_func with cb_data for each directory-entry in the traversal. Do note that
 * not all inode-types are supported, e.g. symlinks are not represented only the types:
 *
 *   * Directory
 *   * Regular file
 *
 * Returns 0 on success. On error, negative errno is returned to indicate the error.
 */
int
xal_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data);

uint64_t
xal_fsbno_offset(struct xal *xal, uint64_t fsbno);

int
xal_inode_path_pp(struct xal *xal, struct xal_inode *inode);

/**
 * Determine if the given inode is a directory
 *
 * @param inode The pointer to the inode
 *
 * @returns True if the inode is a directory
 */
bool
xal_inode_is_dir(struct xal_inode *inode);

/**
 * Determine if the given inode is a regular file
 *
 * @param inode The pointer to the inode
 *
 * @returns True if the inode is a regular file
 */
bool
xal_inode_is_file(struct xal_inode *inode);

/**
 * Retrieve the inode that represent the file or directory at the given path.
 * 
 * If xal is opened with XAL_FILE_LOOKUPMODE_HASHMAP, this will be a constant
 * time lookup. Else, it will search through the tree at xal->root to find the
 * inode.
 * 
 * Note: File system must be mounted and xal opened with backend FIEMAP.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory.
 * @param inode Pointer for the found inode
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_inode(struct xal *xal, char *path, struct xal_inode **inode);

/**
 * Retrieve the extents for the file at the given path.
 * 
 * This will search through the tree at xal->root to find the inode. This call fails if the entry
 * at the given path is not a file.
 * Note: File system must be mounted and xal opened with backend FIEMAP.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory.
 * @param extents Pointer for the found xal_extents
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_extents(struct xal *xal, char *path, struct xal_extents **extents);

/**
 * Retrieve the directory entries for the directory at the given path.
 * 
 * This will search through the tree at xal->root to find the inode. This call fails if the entry
 * at the given path is not a directory.
 * Note: File system must be mounted and xal opened with backend FIEMAP.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory.
 * @param dentries Pointer for the found xal_dentries
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_dentries(struct xal *xal, char *path, struct xal_dentries **dentries);

#endif /* LIBXAL_H */
