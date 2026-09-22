#ifndef XAL_BE_FIEMAP_H
#define XAL_BE_FIEMAP_H

struct xal_reflink;

/**
 * Basename prefix of the per-session reflink shadow directory: <mnt>/.xal_snapshot.<pid>
 *
 * Shared with the inotify watcher, which drops events naming a shadow directory so that the
 * snapshot does not report itself as a change to the filesystem it snapshotted.
 */
#define XAL_SNAPSHOT_PREFIX ".xal_snapshot."

struct xal_be_fiemap {
	struct xal_backend_base base;
	char *mountpoint;      ///< Path to mountpoint of dev
	char *subtree;         ///< Optional absolute path at/under mountpoint to scope the index to; NULL = whole mount
	struct xal_inotify *inotify;
	void *path_inode_map;  ///< Map of paths to inodes

	struct xal_reflink *reflink; ///< Reflink-snapshot state; non-NULL in XAL_WATCHMODE_REFLINK_SNAPSHOT

	uint8_t _rsvd[8];
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_fiemap) == XAL_BACKEND_SIZE, "Incorrect size");

void
xal_be_fiemap_close(struct xal *xal);

int
xal_be_fiemap_open(struct xal **xal, char *mountpoint, struct xal_opts *opts);

int
xal_be_fiemap_get_inode(struct xal *xal, char *path, struct xal_inode **inode);

#endif /* XAL_BE_FIEMAP_H */
