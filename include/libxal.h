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
	/**
	 * Nothing is watched and nothing is pinned.
	 *
	 * The extents are a snapshot of the filesystem as it was at xal_index() time, and a
	 * foreign write can invalidate them at any point. For a caller that owns the filesystem;
	 * see xal_mark_dirty() to signal a change made by the caller itself.
	 */
	XAL_WATCHMODE_NONE = 0,

	/**
	 * Reflink every regular file into a private snapshot at xal_index() time (optionally
	 * restricted to opts.subtree) and capture extents from the clones, so no foreign write
	 * can move a block out from under a published extent.
	 *
	 * The pinning does not mean extents never change; it means only xal_index() can change
	 * them. A foreign write is undetectable in time; a re-snapshot is an event this library
	 * controls and reports by advancing the sequence lock. See xal_get_extents() for the
	 * reader protocol that depends on it, and xal_index() for what a re-snapshot does to the
	 * previous one.
	 *
	 * Reserves a name: entries called ".xal_snapshot.*" are excluded from the index at every
	 * depth, since that is where the clones live, so a file or directory of that name is
	 * invisible to xal_get_inode() and xal_get_extents(). Clones are removed at xal_close().
	 *
	 * At most one handle per mount may use this mode: each removes every ".xal_snapshot.*"
	 * under the mountpoint, another handle's clones included.
	 *
	 * An inotify watch runs alongside; xal_is_dirty() becoming true then means the filesystem
	 * has moved on, not that anything in hand went bad.
	 */
	XAL_WATCHMODE_REFLINK_SNAPSHOT = 1,

	/**
	 * FIEMAP backend only; xal_open() refuses it for XAL_BACKEND_XFS and with
	 * xal_opts.shm_name with -ENOTSUP. xal_index() builds the tree without extents;
	 * xal_extents_acquire() maps a file on first use while holding a read lease on it
	 * (F_SETLEASE, F_RDLCK), so that a writer's open() or truncate() of the file waits until
	 * the callers between xal_extents_acquire() and xal_extents_release() are done. While a
	 * handle in this mode is open, a thread of xal's takes SIGRTMIN+7 and SIGIO; see README.md.
	 * Not 2, which configs written for the removed extent-update mode still name.
	 */
	XAL_WATCHMODE_READ_LEASE = 3,
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
	/** If set, pool memory is backed by POSIX shared memory under this base name; see
	 *  xal_from_shm() for attaching to the pools from another process. */
	const char *shm_name;

	/** FIEMAP backend only: absolute path at or under the mountpoint to scope the index to.
	 *  Only files beneath it are indexed, and in XAL_WATCHMODE_REFLINK_SNAPSHOT only those
	 *  are reflinked. NULL or empty indexes the whole mount. Ignored by the XFS backend. */
	const char *subtree;

	/** XAL_WATCHMODE_READ_LEASE only: if non-zero, xal_extents_release() does not map the file
	 *  again with FIEMAP, and so misses the changes that break no lease; xal_extents_acquire()
	 *  then hands out the old extents until the lease is broken or dropped. See
	 *  xal_extents_release(). */
	int lease_no_recheck;

	/** XAL_WATCHMODE_READ_LEASE only: how many files to keep under a lease. Past it, xal drops
	 *  the lease of a file without readers that was not acquired since the last pass; it is
	 *  exceeded while every leased file has readers, and briefly by files being mapped at the
	 *  same time. 0 for half the soft RLIMIT_NOFILE at xal_open(), or no limit when that is
	 *  unlimited. */
	uint32_t lease_max;
};

/**
 * Bits of xal_extent.flag. They describe the blocks an extent names, and an extent with none of
 * them set holds the file's data in those blocks. After a failed writeback of the file, or a
 * write to it while it was mapped, the extents need not describe the file's data, whatever the
 * bits say; see xal_index().
 *
 * The FIEMAP backend reports all of them. The XFS backend reads the on-disk extent record, which
 * holds only the unwritten state: from it, a clear XAL_EXTENT_SHARED means unknown rather than
 * unshared, and delayed allocations exist only in memory.
 *
 * In XAL_WATCHMODE_REFLINK_SNAPSHOT the extents are those of xal's clones, which hold only the
 * written blocks of their origins: an extent then carries XAL_EXTENT_SHARED while any other file,
 * normally the origin, still shares its blocks, so the bit says nothing about the origin, and the
 * origin's unwritten ranges are holes.
 */
enum xal_extent_flag {
	/** Allocated but never written: the filesystem reads it as zeros, while the blocks hold
	 *  whatever they held before. */
	XAL_EXTENT_UNWRITTEN = 1 << 0,

	/** The blocks are shared with another file, as after a reflink: writing them in place
	 *  changes that file too. */
	XAL_EXTENT_SHARED = 1 << 1,

	/** start_block is no address the file's data can be read or written at raw: a delayed
	 *  allocation, data whose location the filesystem does not report, or data it keeps
	 *  inline, packed into a tail, unaligned, encoded or encrypted. */
	XAL_EXTENT_NOADDR = 1 << 2,
};

struct xal_extent {
	uint64_t start_offset;
	uint64_t start_block;
	uint64_t nblocks;
	uint8_t flag; ///< Bits of enum xal_extent_flag
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

/**
 * Resolve a pool index to the element it names, or NULL when it names nothing
 *
 * NULL means the index is outside the pool. For a reader attached to a published index that
 * means xal_index() rewrote the pools underneath it, so the index it read was never valid.
 * Treat NULL as the signal to re-read under the sequence lock (see xal_get_extents()), not as
 * an error about the file. It is bounds-checked rather than left to fault because a fault
 * never reaches the sequence comparison that would have told the reader to retry.
 *
 * The check covers one element. Check every element of a {idx, count} span, not just the
 * base: idx and count are read separately, so either can be stale.
 */
struct xal_inode *
xal_inode_at(struct xal *xal, uint32_t idx);

/** @see xal_inode_at() */
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
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error:
 *         -ENODATA for an extent with XAL_EXTENT_NOADDR set, whose start_block is no address.
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
 * @return On success a 0 is returned. On error, negative errno is returned to indicate the error:
 *         -ENODATA for an extent with XAL_EXTENT_NOADDR set, whose start_block is no address.
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
 * Returns true when the index is marked dirty: the watch saw the filesystem change, or
 * xal_mark_dirty() was called. See enum xal_watchmode for what that means for extents in hand.
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
 * rejected with -EXDEV, and a mountpoint of / with -EINVAL. The filesystem must stay mounted for
 * as long as the handle is open.
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
 * XAL_BACKEND_FIEMAP writes each file's dirty data back before mapping it, so delayed allocations
 * map to the blocks they got. When a writeback of the file failed, then or earlier, that flush
 * still writes the file back but then reports the failure, which says nothing about the mapping:
 * in XAL_WATCHMODE_NONE the file is then mapped again without it instead of failing the index;
 * in XAL_WATCHMODE_READ_LEASE, instead of failing xal_extents_acquire().
 * Its extents describe its blocks, which for the range that failed need not hold what the page
 * cache does: that range maps as a hole, as unwritten, or as blocks still holding the data from
 * before. fsync() on the file still reports the failure; the flush clears only the error the
 * kernel keeps for legacy callers, as any FIEMAP_FLAG_SYNC does.
 *
 * In XAL_WATCHMODE_NONE, the extents need not describe data written while the file is being
 * mapped: it can, for example, be in no extent, map as a delayed allocation (XAL_EXTENT_NOADDR),
 * as unwritten, or to blocks that hold it only once the write reaches them or, for a
 * copy-on-write, never.
 *
 * In XAL_WATCHMODE_REFLINK_SNAPSHOT the FICLONE that precedes the mapping flushes the file and
 * fails the index on such a failure, once: it clears that error the same way, so the next index
 * maps the file as XAL_WATCHMODE_NONE does after a failed writeback, except that a range
 * unwritten in the file is a hole in its clone. Writes after the FICLONE never reach the clone.
 *
 * When called, any index created from previous calls to xal_index() are cleared.
 *
 * In XAL_WATCHMODE_REFLINK_SNAPSHOT this re-snapshots, and the clones holding the previous
 * index's blocks are destroyed before the new ones are made. Every extent the previous index
 * produced is invalidated, and the blocks behind them stop being pinned. The sequence lock is
 * advanced across the rebuild, which is what lets a reader following the protocol in
 * xal_get_extents() find out; a reader that cached block addresses outside that protocol has no
 * way to.
 *
 * Takes exactly one caller at a time. A second concurrent call on the same handle returns
 * -EBUSY rather than interleaving, and the watch callback installed by xal_watch_filesystem()
 * counts as a caller: a caller that re-indexes from its own thread as well should expect one of
 * the two to be refused. Interleaved indexes are not merely wasteful -- they can produce extents
 * that were never a file's, from a call that reports success.
 *
 * While this runs, an event the watch thread reads counts as a change, even one naming an entry
 * the index skips. A caller that re-indexes from its own thread while a watch runs can therefore
 * cost one extra re-index.
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
 * dirty; see xal_is_dirty().
 *
 * The callback is called from the watch thread; keep it short and thread-safe. It must not call
 * xal_watch_filesystem(), xal_stop_watching_filesystem() or xal_close().
 *
 * A callback that calls xal_index() is a caller of it like any other, so a program that also
 * re-indexes from another thread has two, and xal_index() refuses the second with -EBUSY. Pick
 * one place to re-index.
 *
 * xal_index() arms one inotify watch per directory for any watch mode other than
 * XAL_WATCHMODE_NONE, whether or not xal_watch_filesystem() is ever called. Those watches come
 * out of fs.inotify.max_user_watches, a per-UID budget shared with every other process, and a
 * tree with more directories than the budget has left fails to index with -ENOSPC.
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
 * must be serialised by the caller, including against themselves. None of them may be called
 * from xal_dirty_cb.
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
 * XAL_FILE_LOOKUPMODE_HASHMAP, or after xal_from_shm(), where xal_index() is not
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
 * The result points into the pools -- shared memory for a handle from xal_from_shm() -- and
 * struct xal_extents indexes those pools rather than carrying the block addresses, so this is a
 * reference to live state, not a copy. It is valid for as long as the sequence lock is unchanged.
 * Read it under the lock, and take the second reading after the reads it resolved have completed,
 * not merely after issuing them:
 *
 *     do {
 *             seq = xal_get_seq_lock(xal);
 *             // resolve extents, issue the reads
 *     } while ((seq & 1) || seq != xal_get_seq_lock(xal));
 *
 * An odd first reading is a retry too: the pools were mid-rewrite, and a rewrite that is still
 * running at the second reading leaves the two equal.
 *
 * A re-snapshot frees the blocks behind extents resolved under an earlier sequence number, so an
 * I/O still in flight can land on blocks another file has since been given. The check after
 * completion tells you the data is not trustworthy; it does not prevent the transfer.
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
 * With XAL_WATCHMODE_READ_LEASE, the first call for a file not attached with
 * xal_extents_attach_fd() opens it read-only, takes a read lease on it and maps it as xal_index()
 * does, leaving out extents past the end of the file or cutting them at the block holding it. Each
 * call counts the caller as a reader until xal_extents_release(). A writer's open() or truncate()
 * of the file breaks the lease; the writer waits until the last reader has released, xal then
 * releases the lease, and the next call maps the file again. Between the two calls the extents
 * therefore describe the file's blocks, unless a writeback of the file failed, see xal_index(), or
 * xal_extents_release() returns an error. In other watch modes this returns the indexed extents
 * and pins nothing. README.md lists what the mode needs and what it does not see.
 *
 * Every successful call must be paired with xal_extents_release() on the same inode. Neither, nor
 * xal_extents_attach_fd() or xal_extents_detach_fd(), may run concurrently with xal_index() or
 * xal_close() on the same handle, and xal_index() fails with -EBUSY while extents are held. The
 * leases belong to the process that opened the handle.
 *
 * @param xal The xal struct obtained when opened with xal_open() or xal_open_from_uri()
 * @param inode A regular file in the index
 * @param extents Set to the file's extents on success
 *
 * @return On success, 0. -EAGAIN when the lease cannot be taken (the file is open for writing, or,
 * for a write lease, open elsewhere at all) or is being recalled, -ESTALE when the indexed path now
 * names another file, -EMFILE when xal is out of descriptors and every file holding one has
 * readers, -ECHILD in a forked child, other negative errno on error.
 */
int
xal_extents_acquire(struct xal *xal, struct xal_inode *inode, struct xal_extents **extents);

/**
 * Count the caller out as a reader of a file's extents, and say whether they held its data
 *
 * Call it once every read or DMA through the extents has completed. With
 * XAL_WATCHMODE_READ_LEASE it returns -ESTALE when the kernel has let a writer past the file's
 * broken lease, as it does after lease-break-time, or when an earlier release found the extents
 * stale, or, for a write lease, when the file's size or ctime differ from when it was mapped,
 * which finds writes through the descriptor holding it. Otherwise it maps the file again with
 * FIEMAP and returns -ESTALE when the extents differ, which finds changes that break no lease, at
 * the cost of a FIEMAP per release, see README.md. xal_opts.lease_no_recheck leaves it out. After
 * -ESTALE the file is mapped again on a later xal_extents_acquire(). Not seen: a mapping that
 * changed and changed back, writes to the block device, and a break that xal's thread has not
 * taken yet; when /proc/self/fdinfo cannot be read, as without /proc, one taken more than 250 ms
 * after the kernel issued it, with lease-break-time as last read, 45 s if never. In other watch
 * modes this does nothing.
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
 * Hold a file's lease on the caller's descriptor of it, and map the file now
 *
 * XAL_WATCHMODE_READ_LEASE only. For a caller that opens its files and registers them before
 * reading, as cuFile-like callers do: the open(), F_SETLEASE and first FIEMAP leave the first
 * xal_extents_acquire(). xal takes a read lease when fd is open read-only and a write lease
 * otherwise, takes over fd's F_SETSIG and owner for the break, takes the lease again on fd after a
 * break, and never closes fd. xal_index() and xal_close() release every attachment and leave the
 * descriptors open.
 *
 * fd must stay open, and name that file, until xal_extents_detach_fd(), xal_index() or
 * xal_close(), and its open file may be attached once. A write lease breaks on any other open() of
 * the file, so a thread holding the extents must not open the file itself. Writes through fd break
 * nothing; the next xal_extents_release() returns -ESTALE when the file's size or ctime changed.
 * README.md has the details.
 *
 * @param xal The xal struct obtained when opened with xal_open() or xal_open_from_uri()
 * @param inode A regular file in the index
 * @param fd The caller's descriptor of that file
 * @param attached Set to the inode to pass for the attached file afterwards, which is inode; may be
 *        NULL
 *
 * @return On success, 0. -EAGAIN when the lease cannot be taken now, where a retry may succeed;
 * -EBUSY when the file has readers, is being mapped, dropped or revoked, or is attached, or fd is
 * attached already; -EINVAL when the handle is not in XAL_WATCHMODE_READ_LEASE or fd is not the
 * file's; -ECHILD in a forked child; other negative errno as xal_extents_acquire().
 */
int
xal_extents_attach_fd(struct xal *xal, struct xal_inode *inode, int fd,
		      struct xal_inode **attached);

/**
 * Release the lease xal_extents_attach_fd() took, and leave fd open
 *
 * @return On success, 0. -EBUSY while the file has readers or is being mapped, dropped or revoked,
 * -EINVAL when it is not attached, -ECHILD in a forked child.
 */
int
xal_extents_detach_fd(struct xal *xal, struct xal_inode *inode);

struct xal_lease_stats {
	uint64_t mapped;  ///< Files mapped under a lease, counting re-maps
	uint64_t refused; ///< Refused leases: open for writing, or, for a write lease, elsewhere
	uint64_t breaks;  ///< Lease-break notifications received
	uint64_t revoked; ///< Leases released after a break, or after a release found them stale
	uint64_t rescans; ///< Times every lease was checked because notifications were lost
	uint64_t stale;	  ///< Releases that found the extents may no longer hold the file's data
	uint64_t evicted; ///< Leases dropped on files without readers, to stay within lease_max
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


#endif /* LIBXAL_H */
