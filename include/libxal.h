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
#define XAL_INODE_PATH_MAXLEN 4096
#define XAL_POOL_IDX_NONE UINT32_MAX

enum xal_backend {
	XAL_BACKEND_XFS     = 1,
	XAL_BACKEND_FIEMAP  = 2,
};

enum xal_watchmode {
	XAL_WATCHMODE_NONE             = 0,  ///< There will be no notifications of changes to the filesystem.
	XAL_WATCHMODE_DIRTY_DETECTION  = 1,  ///< When changes to the file system occurs, the xal struct will become "dirty" indicating that the representation of the file system is stale.
	XAL_WATCHMODE_EXTENT_UPDATE    = 2,  ///< When other changes to the file system occurs, the xal struct will be automatically updated if the extent information is the only subject to change, otherwise the xal struct will become "dirty" indicating that the representation of the file system is stale.
	XAL_WATCHMODE_REFLINK_SNAPSHOT = 3,  ///< At xal_index() time, reflink every regular file (optionally restricted to opts.subtree) into a private snapshot and capture extents from the clones. The clones pin their blocks for the xal session, so the extents returned by xal_get_extents() stay valid under concurrent writes. No inotify watch or filesystem freeze is used; clones are removed at xal_close().
	XAL_WATCHMODE_READ_LEASE = 4,        ///< FIEMAP backend only. xal_index() builds the tree without extents; xal_extents_acquire() maps a file on first use while holding a read lease on it (F_SETLEASE, F_RDLCK), so that a writer's open() or truncate() of the file waits until the callers between xal_extents_acquire() and xal_extents_release() are done. See xal_extents_acquire() for the requirements.
};

enum xal_file_lookupmode {
	XAL_FILE_LOOKUPMODE_TRAVERSE = 0,  ///< Traverses the file tree from xal->root using binary search at each level to find the inode.
	XAL_FILE_LOOKUPMODE_HASHMAP = 1,   ///< Uses a hash map for constant-time inode lookup in xal_get_inode(), with higher memory usage.
};

struct xal_opts {
	enum xal_backend be;
	enum xal_watchmode watch_mode;
	enum xal_file_lookupmode file_lookupmode;
	const char *mountpoint;
	const char *shm_name; ///< If set, pool memory is backed by POSIX shared memory with this base name, see @xal_from_pools() for sharing the pools across processes
	const char *subtree; ///< FIEMAP backend only: absolute path at or under the mountpoint to scope the index to. Only files under it are indexed (and, in XAL_WATCHMODE_REFLINK_SNAPSHOT, reflinked). NULL/empty indexes the whole mount. Ignored by the XFS backend.
	int lease_no_recheck; ///< XAL_WATCHMODE_READ_LEASE only: if non-zero, xal_extents_release() does not map the file again with FIEMAP, and so misses the changes that break no lease; xal_extents_acquire() then hands out the old extents until the lease is broken or dropped. See xal_extents_release().
	uint32_t lease_max; ///< XAL_WATCHMODE_READ_LEASE only: how many files to keep under a lease. Past it, xal drops the lease of a file without readers that was not acquired since the last pass; it is exceeded while every leased file has readers, and briefly by files being mapped at the same time. 0 for half the soft RLIMIT_NOFILE at xal_open(), or no limit when that is unlimited.
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
	char name[XAL_INODE_NAME_MAXLEN + 1]; ///< Leaf name of the entry, nul-terminated; not a path, see xal_inode_path()
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
 * @note If the xal struct was not opened with backend "fiemap", change detection is not supported.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * 
 * @return a boolean value, indicating whether breaking changes to the file-system have
 *         been found.
 */
bool
xal_is_dirty(struct xal *xal);

/**
 * Mark the xal representation as dirty.
 *
 * Forces the dirty flag without waiting for the filesystem watcher, so a caller
 * that just changed the filesystem (e.g. an allocating write) can guarantee the
 * next extent resolution sees it as stale and re-indexes. Idempotent.
 *
 * Safe to call from any thread at any time; a mark that lands while xal_index() is
 * rebuilding the index survives it, so the representation stays dirty and the change
 * is observed by a further re-index.
 *
 * @param xal The xal struct obtained when opened with xal_open()
 */
void
xal_mark_dirty(struct xal *xal);

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
 * When opts->shm_name is set, the pools are published under that name for other processes to
 * read, and this process is the one expected to call xal_index(). The name must not already
 * exist: the regions are created with O_EXCL and outlive their creator, so -EEXIST means the
 * name is taken, by a live holder or by a remnant of one that died without unlinking. Readers
 * attach with xal_from_shm() rather than by opening the device again.
 *
 * opts->be is in/out: left zero it is filled in with the detected backend and stays set. A caller
 * reusing one struct xal_opts across devices must clear it between calls.
 *
 * With XAL_BACKEND_FIEMAP, the mountpoint and opts->subtree must be directories, not symlinks to
 * one; -ENOTDIR is returned otherwise. A subtree on another filesystem than the mountpoint is
 * rejected with -EXDEV, and a mountpoint of / with -EINVAL.
 *
 * @param dev Pointer to xnvme device handled as retrieved with xnvme_dev_open()
 * @param xal Pointer
 * @param opts Pointer to options, see xal_opts
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts);

/**
 * Open and decode the file-system meta-data of the mount holding the given device
 *
 * As xal_open(), but with no xnvme device. FIEMAP is then the only reachable backend, and
 * sb.lba_blksze is left unset, so xal_extent_in_lba() returns -EINVAL on the resulting handle.
 *
 * @param uri The device to index; it must be mounted, unless opts->mountpoint says where
 * @param xal Pointer
 * @param opts Pointer to options, see xal_opts
 *
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_open_from_uri(const char *uri, struct xal **xal, struct xal_opts *opts);

void
xal_close(struct xal *xal);

/**
 * Retrieve inodes from disk and decode the on-disk-format of the retrieved data
 *
 * Only the process that builds the index may call this; it returns -EINVAL on a handle obtained
 * from xal_from_shm(). This holds for both backends, also XAL_BACKEND_FIEMAP where the call
 * otherwise does nothing.
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
 * Only directories and regular files are indexed; symlinks and special files are skipped, and so
 * is anything mounted inside the tree with XAL_BACKEND_FIEMAP. That backend also checks the
 * mountpoint and opts->subtree again, as xal_open() does.
 * 
 * When called, any index created from previous calls to xal_index() are cleared.
 * 
 * This function will fail if given a xal handle obtained from xal_from_shm(), and with
 * XAL_WATCHMODE_READ_LEASE it fails with -EBUSY while extents are held, see xal_extents_acquire().
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_index(struct xal *xal);

/**
 * Callback invoked by the background watch thread when it observes the xal struct becoming
 * dirty. Dirty means breaking filesystem changes (file creation, deletion, or rename) were
 * detected or marked via xal_mark_dirty(), and the in-memory representation is now stale.
 *
 * The callback is called from the watch thread; keep it short and thread-safe. It must not call
 * xal_watch_filesystem(), xal_stop_watching_filesystem() or xal_close().
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
 * This call, xal_stop_watching_filesystem() and xal_close() all act on the same watch thread and
 * must be serialised by the caller, including against themselves: two concurrent starts can each
 * create a thread and leave one of them unjoinable, and two concurrent stops can join the same
 * thread twice. None of them may be called from xal_dirty_cb, since that runs on the watch thread
 * and would join itself.
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
 *  - the background thread has been started, see `xal_watch_filesystem()`,
 *  - the background thread has not previously been stopped and joined by `xal_close()` or 
 *    `xal_stop_watching_filesystem()`.
 * 
 * If these assumptions do not hold, this will result in an error.
 * 
 * Requires the same external serialisation as xal_watch_filesystem().
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_stop_watching_filesystem(struct xal *xal);

/**
 * Recursively walk the given directory
 *
 * Invoking the given cb_func with cb_data for each directory-entry in the traversal. Only
 * directories and regular files are indexed, see xal_index().
 *
 * The index is checked before the traversal starts: -ESTALE is returned when it has been marked
 * dirty, which for a handle from xal_from_shm() includes the primary having closed. The check is
 * not repeated during the walk, and xal_get_root() and xal_inode_at() do not perform it at all.
 *
 * Returns 0 on success. On error, negative errno is returned to indicate the error.
 */
int
xal_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data);

uint64_t
xal_fsbno_offset(struct xal *xal, uint64_t fsbno);

/**
 * Construct a read-only xal from shared memory created by a primary process.
 *
 * Intended for multi-process support, where a primary process has built the xal tree with
 * opts->shm_name set and called xal_index(). All metadata (superblock, backend type, mountpoint,
 * the subtree a scoped index is rooted at, root index) is read from the shared state region; no
 * out-of-band communication is required.
 * The resulting xal can be closed with xal_close(), which will free the struct xal allocation and
 * munmap the shared memory regions.
 *
 * Calling xal_get_inode(), xal_get_extents() or xal_get_dentries() on the returned xal struct
 * will always result in it using the XAL_FILE_LOOKUPMODE_TRAVERSE. To get constant time lookups
 * with the XAL_FILE_LOOKUPMODE_HASHMAP mode, call xal_build_lookup_hashmap() before.
 *
 * The state region carries a magic and a version, checked here along with its size; the version
 * covers the pool regions too. -EPROTO means the primary published from a build this one cannot
 * read, which no amount of waiting fixes; -EAGAIN means the primary has not finished publishing
 * yet.
 *
 * @param shm_name	   Base name for the shared memory regions (same value as opts->shm_name)
 * @param out          Output pointer for the constructed xal
 *
 * @return On success, 0. On error, negative errno.
 */
int
xal_from_shm(const char *shm_name, struct xal **out);

/**
 * Print the path of the given inode to stdout
 *
 * The path is printed relative to the root of the indexed tree, that is, without the basepath
 * that xal_inode_path() prefixes for the FIEMAP backend, and nothing is printed for the root
 * itself. That is what lets a caller prefix a root of its own; 'xal --find' prints the device
 * URI.
 *
 * @param xal The xal struct obtained when opened with xal_open()
 * @param inode The inode to print the path of
 *
 * @return The number of characters printed.
 */
int
xal_inode_path_pp(struct xal *xal, struct xal_inode *inode);

/**
 * Assemble the absolute path of the given inode into the given buffer.
 *
 * 'struct xal_inode' stores the leaf name of the entry; the path is assembled by walking the
 * parent chain to the root of the indexed tree. For the FIEMAP backend it is prefixed with the
 * indexed root on the local filesystem (the mountpoint, or opts.subtree when given), for the XFS
 * backend it is given as if the filesystem was mounted at root ("/").
 *
 * @param xal The xal struct obtained when opened with xal_open()
 * @param inode The inode to assemble the path of
 * @param buf Buffer receiving the nul-terminated path; XAL_INODE_PATH_MAXLEN + 1 bytes holds any
 * path the FIEMAP backend can index, that being a path on a mounted filesystem. The XFS backend
 * indexes trees of any depth, so a path there can be longer, and -ENAMETOOLONG is returned
 * @param buf_nbytes Size of 'buf' in bytes, including room for nul-termination
 *
 * @return On success, the length of the path written, not counting nul-termination, is returned.
 * On error, negative errno is returned to indicate the error.
 */
int
xal_inode_path(struct xal *xal, struct xal_inode *inode, char *buf, size_t buf_nbytes);

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
 * In hashmap mode a lookup racing xal_index() in another thread of the same process can compare
 * against a key that the re-index has already freed. Serialising the two avoids it.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory. If opened with the XFS backend, the path should
			   be given as if it was mounted at root ("/").
 * @param inode Pointer for the found inode
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_inode(struct xal *xal, char *path, struct xal_inode **inode);

/**
 * Build a path-to-inode hash map from the in-memory inode tree.
 *
 * Intended for use after xal_open() without opts->file_lookupmode not set to 
 * XAL_FILE_LOOKUPMODE_HASHMAP, or after xal_from_pools(), where xal_index() is not
 * called but the caller wants constant-time inode lookup via xal_get_inode(). Walks
 * the existing tree and populates the hash map locally. Any previously existing
 * map is replaced.
 *
 * Note: xal must use backend FIEMAP.
 *
 * @param xal The xal struct
 *
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_build_lookup_hashmap(struct xal *xal);

/**
 * Retrieve the extents for the file at the given path.
 * 
 * This will search through the tree at xal->root to find the inode. This call fails if the entry
 * at the given path is not a file.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory. If opened with the XFS backend, the path should
			   be given as if it was mounted at root ("/").
 * @param extents Pointer for the found xal_extents
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_extents(struct xal *xal, char *path, struct xal_extents **extents);

/**
 * Pin the extents of a regular file for raw reads
 *
 * With XAL_WATCHMODE_READ_LEASE, the first call for a file opens it read-only, takes a read lease
 * on it and maps it with FIEMAP_FLAG_SYNC, so dirty data is written back first. The extents then
 * cover the file's data up to its size: unwritten extents are left out, as they read as zeros like
 * holes, and extents past the end of the file are left out or cut at the block holding it. Each
 * call counts the caller as a reader
 * until xal_extents_release(). A writer's open() or truncate() of the file breaks the lease; the
 * writer waits until the last reader has released, xal then releases the lease, and the next call
 * maps the file again. Between the two calls the extents therefore hold the file's data, unless
 * xal_extents_release() returns an error. In other watch modes this returns the indexed extents
 * and pins nothing.
 *
 * Every successful call must be paired with xal_extents_release() on the same inode. Neither may
 * run concurrently with xal_index() or xal_close() on the same handle, and xal_index() fails with
 * -EBUSY while extents are held.
 *
 * Requirements and limits of XAL_WATCHMODE_READ_LEASE:
 *  - The caller owns the files or has CAP_LEASE, and /proc/sys/fs/leases-enable is 1.
 *  - Every mapped file holds a file descriptor while it has a lease. Past xal_opts.lease_max
 *    leases, or when xal runs out of descriptors, for a file or a release's check, it drops the
 *    lease of a file without readers that was not acquired since the last pass, and maps that
 *    file again on its next acquire. This returns -EMFILE only when every leased file has
 *    readers.
 *  - The kernel lets a waiting writer through after /proc/sys/fs/lease-break-time seconds (45 by
 *    default) even if readers remain, so a reader must release within that time. A reader that
 *    releases later gets -ESTALE from xal_extents_release(), once xal's thread has taken the
 *    break. xal_extents_recalled() tells a reader that a writer waits and how long is left. A
 *    lease-break-time of 0 makes the kernel wait for the readers however long they take; it
 *    applies to every lease on the system.
 *  - A writer that opens with O_NONBLOCK, as coreutils truncate does, does not wait: it gets
 *    EWOULDBLOCK while any lease is held on the file, and succeeds on a retry once xal has
 *    released it.
 *  - Breaks are reported to a thread of xal's, which blocks all signals and takes SIGRTMIN+7 and
 *    SIGIO with sigtimedwait(). No signal disposition changes, but that thread may also take
 *    those signals when they are sent to the process, so the caller must not use SIGRTMIN+7 or
 *    rely on SIGIO sent to the process while a handle in this mode is open.
 *  - The leases belong to the process that opened the handle. In a forked child this returns
 *    -ECHILD, and xal_index() fails with -ECHILD. A child is told apart by pthread_atfork(), so
 *    one made without the fork handlers, as by clone() or _Fork(), is not. Such a child must not
 *    use or close the handle, as it would release the parent's leases. A forked child closes the
 *    leased files it inherits at fork(). fork() waits for xal's locks, which xal_open(),
 *    xal_close(), xal_index(), xal_extents_acquire(), xal_extents_release() and xal_lease_stats()
 *    hold for short spans, so do not call these while holding a lock that a fork handler of the
 *    caller takes, nor call fork() from a signal handler while a handle in this mode is open. A
 *    file being mapped at fork(), or whose lease another thread is dropping, stays open in the
 *    child, even after xal_close() there, until it exits or execs.
 *  - Only changes that open the file for writing or truncate it break the lease. Others are not
 *    prevented: a dedupe (FIDEDUPERANGE) through a read-only descriptor, which the file's owner,
 *    anyone with write permission on it, or CAP_SYS_ADMIN may do; data moved by the filesystem
 *    itself, as zoned XFS garbage collection does. The blocks read before may meanwhile have been
 *    given to another file. xal_extents_release() should find such changes and return -ESTALE
 *    (tested with a dedupe), unless xal_opts.lease_no_recheck is set. Writes to the block device
 *    itself are not seen at all.
 *  - Not supported together with xal_opts.shm_name.
 *  - xal_get_extents() and xal_walk() report no extents for a file until it has been acquired,
 *    nor after its lease has been broken or dropped.
 *
 * @param xal The xal struct obtained when opened with xal_open() or xal_open_from_uri()
 * @param inode A regular file in the index
 * @param extents Set to the file's extents on success
 *
 * @return On success, 0. -EAGAIN when the file is open for writing or its lease is being
 * recalled, -ESTALE when the indexed path now names another file, -EOPNOTSUPP when part of the
 * file is not stored as is on the device (delayed allocation, inline, encoded or encrypted data),
 * -ECHILD in a forked child, other negative errno on error.
 */
int
xal_extents_acquire(struct xal *xal, struct xal_inode *inode, struct xal_extents **extents);

/**
 * Count the caller out as a reader of a file's extents, and say whether they held its data
 *
 * Call it once every read or DMA through the extents has completed. With
 * XAL_WATCHMODE_READ_LEASE it returns -ESTALE when the kernel has let a writer past the file's
 * broken lease, as it does after lease-break-time, or when an earlier release found the extents
 * stale. Otherwise it maps the file again with FIEMAP and returns -ESTALE when the extents differ,
 * which finds changes that break no lease; this costs a FIEMAP per release, which grows with the
 * number of extents and, on XFS, takes the AGF buffer lock and a refcount btree lookup per extent
 * of a file that has ever shared blocks, so that releases of such files in one allocation group
 * wait for each other. xal_opts.lease_no_recheck leaves it out. After -ESTALE the file is mapped
 * again on a later xal_extents_acquire(). Not seen: a mapping that changed and changed back, writes
 * to the block device, and a break that xal's thread has not taken yet; when /proc/self/fdinfo
 * cannot be read, as without /proc, one taken more than 250 ms after the kernel issued it, with
 * lease-break-time as last read, 45 s if never. In other watch modes this does nothing.
 *
 * @param xal The xal struct obtained when opened with xal_open() or xal_open_from_uri()
 * @param inode The inode passed to xal_extents_acquire()
 *
 * @return On success, 0. -ESTALE when the data read through the extents may not be the file's,
 * -EINVAL when the file has no reader to count out, -ECHILD in a forked child, other negative
 * errno when the check could not be made. Except with -EINVAL and -ECHILD the caller is counted
 * out, and on any error it must not trust what it read.
 */
int
xal_extents_release(struct xal *xal, struct xal_inode *inode);

/**
 * Say whether the readers of a file's extents should release them
 *
 * With XAL_WATCHMODE_READ_LEASE, a writer that opens or truncates a held file waits for its
 * readers, and the kernel lets it through anyway after lease-break-time. A reader that holds the
 * extents for long can call this between reads and, once it returns 1, stop reading through them
 * and release them, so that the writer goes ahead; xal_extents_acquire() then returns -EAGAIN
 * until the writer is done. The call takes no lock and makes no system call, except to read
 * CLOCK_MONOTONIC for left_ns while the extents are being revoked. In other watch modes this
 * returns 0.
 *
 * @param xal The xal struct obtained when opened with xal_open() or xal_open_from_uri()
 * @param inode A regular file in the index
 * @param left_ns If not NULL, set to the nanoseconds left before the kernel may let the writer
 * through, UINT64_MAX when it never does, and 0 when a release found the extents stale or this
 * returns 0
 *
 * @return 1 when the extents are being revoked, because a writer waits or a release found them
 * changed; 0 when not; -EINVAL on a bad argument, -ECHILD in a forked child.
 */
int
xal_extents_recalled(struct xal *xal, struct xal_inode *inode, uint64_t *left_ns);

struct xal_lease_stats {
	uint64_t mapped;   ///< Files mapped under a lease, counting re-maps
	uint64_t unsealed; ///< Lease attempts refused because the file was open for writing
	uint64_t breaks;   ///< Lease-break notifications received
	uint64_t revoked;  ///< Leases released after a break, or after a release found them stale
	uint64_t rescans;  ///< Times every lease was checked because notifications were lost
	uint64_t stale;    ///< Releases that found the extents may no longer hold the file's data
	uint64_t evicted;  ///< Leases dropped on files without readers, to stay within lease_max
};

/**
 * Counters of XAL_WATCHMODE_READ_LEASE; -EINVAL in other watch modes
 */
int
xal_lease_stats(struct xal *xal, struct xal_lease_stats *stats);

/**
 * Retrieve the directory entries for the directory at the given path.
 * 
 * This will search through the tree at xal->root to find the inode. This call fails if the entry
 * at the given path is not a directory.
 * 
 * @param xal The xal struct obtained when opened with xal_open()
 * @param path Absolute path to the file or directory. If opened with the XFS backend, the path should
			   be given as if it was mounted at root ("/").
 * @param dentries Pointer for the found xal_dentries
 * 
 * @returns On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_get_dentries(struct xal *xal, char *path, struct xal_dentries **dentries);

#ifdef XAL_BPF_ENABLED
/**
 * Start a background thread polling the BPF ring buffer for filesystem events
 * (e.g. unfreeze) on the block device.
 *
 * Assumes that
 *  - you have run xal_open() with backend FIEMAP and BPF event listening enabled,
 *  - the BPF skeleton and ring buffer have been initialized.
 *
 * If the thread is already running, this is a no-op and 0 is returned.
 *
 * @param xal The xal struct obtained when opened with xal_open().
 *
 * @returns On success, 0 is returned. On error, a negative errno is returned to indicate the
 *          error: -EINVAL if xal is NULL; -ENOTSUP if the backend is not FIEMAP; -ENODEV if xal
 *          was not opened with BPF event listening; -ENOTCONN if the BPF skeleton is not
 *          initialized; -ENOBUFS if the ring buffer is not initialized; or the negated errno
 *          from pthread_create() if thread creation fails.
 */
int
xal_bpf_start_poll_thread(struct xal *xal);

/**
 * Stop the background thread polling the BPF ring buffer.
 *
 * Assumes that
 *  - you have run xal_open() with backend FIEMAP and BPF event listening enabled,
 *  - the background thread is running, see xal_bpf_start_poll_thread().
 *
 * If these assumptions do not hold, this will result in an error.
 *
 * @param xal The xal struct obtained when opened with xal_open().
 *
 * @returns On success, 0 is returned. On error, a negative errno is returned to indicate the
 *          error: -EINVAL if xal is NULL; -ENOTSUP if the backend is not FIEMAP; -ENODEV if xal
 *          was not opened with BPF event listening; -ENOTCONN if the BPF skeleton is not
 *          initialized; -ESRCH if the background thread is not running; or the negated errno
 *          from pthread_join() if joining the thread fails.
 */
int
xal_bpf_stop_poll_thread(struct xal *xal);
#endif /* XAL_BPF_ENABLED */

#endif /* LIBXAL_H */
