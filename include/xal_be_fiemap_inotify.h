#ifndef XAL_BE_FIEMAP_INOTIFY_H
#define XAL_BE_FIEMAP_INOTIFY_H

#include <stdatomic.h>

#define XAL_BE_FIEMAP_INOTIFY_JOINABLE 1
#define XAL_BE_FIEMAP_INOTIFY_EXITED 2

struct xal_inotify {
	enum xal_watchmode watch_mode;
	int fd;           ///< inotify descriptor, or -1 when no watch mode is in use
	void *inode_map;  ///< Set of watch descriptors held by this instance
	pthread_t watch_thread_id;
	atomic_int flag;
	atomic_bool stop;

	/**
	 * Serialises starting the watch thread against reaping it.
	 *
	 * watch_thread_id is a plain pthread_t written by pthread_create() and read by every
	 * reaper, so the flags alone cannot make the pair consistent: whichever side of
	 * pthread_create() they are raised on, a reaper can observe one without the other and
	 * either skip a join it owed or join an id nothing has written. Publishing the id and
	 * the flags in one critical section removes that choice.
	 */
	pthread_mutex_t lifecycle;

	xal_dirty_cb cb;
	void *cb_args;
};

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify);

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode);

/**
 * Drain and discard all pending events from the inotify file descriptor.
 *
 * xal_index() calls this after xal_be_fiemap_inotify_clear_inode_map() and before the re-walk,
 * to discard what accumulated before the rebuild, including the IN_IGNORED each removal
 * queues. Events that arrive during the re-walk stay queued for the watch thread.
 *
 * @param inotify  Pointer to the xal_inotify struct.
 */
int
xal_be_fiemap_inotify_drain(struct xal_inotify *inotify);

/**
 * Drop every watch this instance holds, both the kernel watch and the map entry.
 *
 * Run before the drain in xal_index(), since each removal queues an IN_IGNORED. The walk
 * re-adds a watch for every directory it reads.
 *
 * @param inotify  Pointer to the xal_inotify struct.
 */
int
xal_be_fiemap_inotify_clear_inode_map(struct xal_inotify *inotify);

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path);

#endif /* XAL_BE_FIEMAP_INOTIFY_H */
