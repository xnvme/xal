#define XAL_BE_FIEMAP_INOTIFY_RUNNING 1

struct xal_inotify {
	enum xal_watchmode watch_mode;
	int fd;           ///< File descriptor for inotify events, if opened with some xal_watchmode, else 0
	void *inode_map;  ///< Map of inodes from inotify watch descriptors
	pthread_t watch_thread_id;
	int flag;
	xal_dirty_cb cb;
	void *cb_args;
};

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify);

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode);

/**
 * Clear the watch descriptor to inode hash table on the given xal_inotify struct.
 * 
 * This is to be used when running xal_index() to ensure that the table points to
 * the correct inodes and none other.
 * 
 * @param inotify  Pointer to the xal_inotify struct.
 */
int
xal_be_fiemap_inotify_clear_inode_map(struct xal_inotify *inotify);

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path, struct xal_inode *inode);
