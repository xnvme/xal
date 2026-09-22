#include <asm-generic/errno.h>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fs.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_inotify.h>

#define XAL_INOTIFY_NOCHANGE 0
#define XAL_INOTIFY_REINDEX 1

/* Bounds how long the watch thread sleeps when it has nothing to do. It also bounds how long
 * xal_be_fiemap_inotify_close() waits in pthread_join(), and how late an externally marked
 * dirty flag is noticed, so it trades those two against the wakeup rate. */
#define XAL_INOTIFY_POLL_TIMEOUT_MS 100

KHASH_MAP_INIT_INT64(wd_to_inode, struct xal_inode *);

/**
 * Take ownership of the watch thread for joining, under the lifecycle lock
 *
 * Returns true and fills @tid for the caller that takes JOINABLE, so two reapers cannot both
 * join the same thread. The join is left to the caller and happens outside the lock: a stop can
 * take up to XAL_INOTIFY_POLL_TIMEOUT_MS to be noticed.
 */
static bool
inotify_claim_join(struct xal_inotify *inotify, bool request_stop, pthread_t *tid)
{
	bool claimed = false;

	pthread_mutex_lock(&inotify->lifecycle);

	if (atomic_load(&inotify->flag) & XAL_BE_FIEMAP_INOTIFY_JOINABLE) {
		atomic_fetch_and(&inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_JOINABLE);
		if (request_stop) {
			atomic_store(&inotify->stop, true);
		}
		*tid = inotify->watch_thread_id;
		claimed = true;
	}

	pthread_mutex_unlock(&inotify->lifecycle);

	return claimed;
}

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify)
{
	kh_wd_to_inode_t *inode_map;
	pthread_t tid;

	if (!inotify) {
		XAL_DEBUG("SKIPPED: No xal_inotify given")
		return;
	}

	inode_map = inotify->inode_map;

	/* Reap whether it was told to stop or exited on its own: a thread that has already exited
	 * ignores the stop. */
	if (inotify_claim_join(inotify, true, &tid)) {
		pthread_join(tid, NULL);
		atomic_fetch_and(&inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_RUNNING);
	}

	if (inode_map) {
		kh_destroy(wd_to_inode, inode_map);
	}

	/* fd 0 is a valid descriptor, so -1 is what means "none". */
	if (inotify->fd >= 0) {
		close(inotify->fd);
		inotify->fd = -1;
	}

	/* Last, and only safe because the caller owns the handle by now: this frees the lock that
	 * guards the start path. A concurrent xal_watch_filesystem() is a use-after-free of the
	 * handle -- bad usage. */
	pthread_mutex_destroy(&inotify->lifecycle);
}

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode)
{
	int err;

	if (!inotify) {
		XAL_DEBUG("FAILED: No xal_inotify given");
		return -EINVAL;
	}

	inotify->watch_mode = watch_mode;
	atomic_init(&inotify->flag, 0);
	atomic_init(&inotify->stop, false);

	err = pthread_mutex_init(&inotify->lifecycle, NULL);
	if (err) {
		XAL_DEBUG("FAILED: pthread_mutex_init(); err(%d)", err);
		return -err;
	}

	/* calloc() leaves this 0, which names stdin rather than nothing. */
	inotify->fd = -1;

	if (!inotify->watch_mode) {
		XAL_DEBUG("INFO: Skipping xal_be_fiemap_inotify_init(), watch mode none given");
		return 0;
	}

	inotify->fd = inotify_init1(IN_NONBLOCK);
	if (inotify->fd < 0) {
		XAL_DEBUG("FAILED: inotify_init1(); errno(%d)", errno);
		return -errno;
	}

	inotify->inode_map = kh_init(wd_to_inode);
	if (!inotify->inode_map) {
		XAL_DEBUG("FAILED: kh_init()");
		return -EINVAL;
	}

	return 0;
}

int
xal_be_fiemap_inotify_drain(struct xal_inotify *inotify)
{
	char buf[4096];
	ssize_t len;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given");
		return -EINVAL;
	}

	do {
		len = read(inotify->fd, buf, sizeof(buf));
	} while (len > 0);

	return 0;
}

int
xal_be_fiemap_inotify_clear_inode_map(struct xal_inotify *inotify)
{
	khash_t(wd_to_inode) *inode_map;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given");
		return -EINVAL;
	}

	inode_map = inotify->inode_map;

	/* Drop the kernel watches, not just the map entries: this stops a departed directory
	 * from reporting, and from leaking its fs.inotify.max_user_watches slot. */
	for (khiter_t k = kh_begin(inode_map); k != kh_end(inode_map); ++k) {
		if (!kh_exist(inode_map, k)) {
			continue;
		}
		if (inotify_rm_watch(inotify->fd, (int)kh_key(inode_map, k)) && (errno != EINVAL)) {
			/* EINVAL is the normal end of a watch on a deleted directory. */
			XAL_DEBUG("FAILED: inotify_rm_watch(%d); errno(%d)",
				  (int)kh_key(inode_map, k), errno);
		}
	}

	kh_clear(wd_to_inode, inode_map);

	return 0;
}

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path, struct xal_inode *inode)
{
	khash_t(wd_to_inode) *inode_map;
	/* IN_MOVE_SELF and IN_DELETE_SELF report on the watched directory itself. Without them the
	 * index root's disappearance is invisible: every other directory is covered by the watch
	 * on its parent, and nothing watches above the root. */
	uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
			IN_MOVE_SELF | IN_DELETE_SELF | IN_UNMOUNT;
	khiter_t iter;
	int wd, err;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given.");
		return -EINVAL;
	}

	inode_map = inotify->inode_map;

	if (!xal_inode_is_dir(inode)) {
		XAL_DEBUG("FAILED: cannot process directory at path(%s) - not a directory", path);
		return -EINVAL;
	}

	if (inotify->watch_mode) {
		wd = inotify_add_watch(inotify->fd, path, mask);
		if (wd < 0) {
			XAL_DEBUG("FAILED: inotify_add_watch(); errno(%d)", errno);
			return -errno;
		}

		iter = kh_put(wd_to_inode, inode_map, wd, &err);
		if (err < 0) {
			XAL_DEBUG("FAILED: kh_put()");
			return -EIO;
		}
		kh_value(inode_map, iter) = inode;
	}

	return 0;
}

static __attribute__((unused)) int
inotify_event_mask_pp(uint32_t mask, char *str, int str_sz) {
	int wrtn, idx = 0;

	if (mask & IN_MODIFY) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_MODIFY");
		idx += wrtn;
	}
	if (mask & IN_ATTRIB) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_ATTRIB");
		idx += wrtn;
	}
	if (mask & IN_CREATE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_CREATE");
		idx += wrtn;
	}
	if (mask & IN_DELETE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_DELETE");
		idx += wrtn;
	}
	if (mask & IN_MOVE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_MOVE");
		idx += wrtn;
	}
	if (mask & IN_MOVE_SELF) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_MOVE_SELF");
		idx += wrtn;
	}
	if (mask & IN_DELETE_SELF) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_DELETE_SELF");
		idx += wrtn;
	}
	if (mask & IN_IGNORED) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_IGNORED");
		idx += wrtn;
	}
	if (mask & IN_ISDIR) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_ISDIR");
		idx += wrtn;
	}
	if (mask & IN_CLOSE_WRITE) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_CLOSE_WRITE");
		idx += wrtn;
	}
	if (mask & IN_UNMOUNT) {
		wrtn = snprintf(str + idx, str_sz - idx, "%s", " IN_UNMOUNT");
		idx += wrtn;
	}

	return wrtn;
}

/**
 * Drain the inotify queue and report whether the index must be rebuilt
 *
 * @return On success XAL_INOTIFY_NOCHANGE or XAL_INOTIFY_REINDEX is returned, the latter asking
 * the caller for a full re-index. On error, negative errno is returned and the watch is over.
 */
static int
check_events(struct xal_inotify *inotify)
{
	char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	ssize_t len, i;

	len = read(inotify->fd, buf, sizeof buf);
	while (len > 0) {
		i = 0;

		while (i < len) {
			struct inotify_event *event = (struct inotify_event *)&buf[i];
			__attribute__((unused)) char mask_pp[128];

			XAL_DEBUG_FCALL(inotify_event_mask_pp, event->mask, mask_pp, 128);
			/* len is 0 for events about the watched directory itself. */
			XAL_DEBUG("INFO: mask(%s) for event with wd(%d) and name(%s)", &mask_pp[1], event->wd,
				  event->len ? event->name : "(none)")

			/* Events were dropped: wd is -1 and no name is carried, so nothing here can
			 * say what changed. Checked first -- no watch mode can do better than a
			 * re-index. */
			if (event->mask & IN_Q_OVERFLOW) {
				XAL_DEBUG("INFO: inotify queue overflowed; events were lost");
				return XAL_INOTIFY_REINDEX;
			}

			/* A watch descriptor is gone, not a filesystem change: this follows every
			 * inotify_rm_watch(). A directory deleted under a watch is already
			 * reported by IN_DELETE on the parent, or IN_DELETE_SELF when it was the
			 * index root. */
			if (event->mask & IN_IGNORED) {
				i += sizeof(struct inotify_event) + event->len;
				continue;
			}

			/* The only fatal one: the filesystem is gone, so there is neither
			 * anything left to watch nor anything a re-index could recover. */
			if (event->mask & IN_UNMOUNT) {
				XAL_DEBUG("FAILED: File system has been unmounted");
				return -EINVAL;
			}

			/* Our own snapshot work lands in the watched mountpoint directory. What
			 * happens inside a shadow dir is invisible -- the walk never descends into
			 * one -- so dropping the event by name is what keeps a snapshot from
			 * reporting itself as a change. */
			if ((inotify->watch_mode == XAL_WATCHMODE_REFLINK_SNAPSHOT) && event->len &&
			    (strncmp(event->name, XAL_SNAPSHOT_PREFIX,
				     sizeof(XAL_SNAPSHOT_PREFIX) - 1) == 0)) {
				XAL_DEBUG("INFO: ignoring own snapshot dir; name(%s)", event->name);
				i += sizeof(struct inotify_event) + event->len;
				continue;
			}

			/* Every surviving event means the same thing. The clones hold the blocks,
			 * so an event cannot invalidate extents a reader already has; it only says
			 * a re-snapshot would see something different. No per-event incremental
			 * path: re-reading extents for the changed file would take them from the
			 * unpinned origin. */
			XAL_DEBUG("INFO: file system moved on under the snapshot");

			return XAL_INOTIFY_REINDEX;
		}

		len = read(inotify->fd, buf, sizeof buf);
	}

	return XAL_INOTIFY_NOCHANGE;
}

static void *
background_thread_start(void *arg)
{
	struct xal *xal = arg;
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct pollfd pfd;
	int cb_seq = -1;
	int err = 0;

	XAL_DEBUG("INFO: starting background thread");

	if (!be->inotify) {
		XAL_DEBUG("FAILED: inotify not initialised, exit thread");
		goto exit_thread;
	}

	pfd.fd = be->inotify->fd;
	pfd.events = POLLIN;

	while (!atomic_load(&be->inotify->stop)) {
		int state = atomic_load(xal->index_state);

		if (state == XAL_STATE_INDEXING) {
			/* Someone else is rebuilding the pools. Nothing here can make that
			 * finish sooner, so wait rather than spin on the state. */
			poll(NULL, 0, XAL_INOTIFY_POLL_TIMEOUT_MS);
			continue;
		}

		if (state == XAL_STATE_DIRTY) {
			/* Notify at most once per tree version: fire only when seq is stable
			 * and differs from the version the cb last fired at, so a mark that
			 * survived an index this loop never observed still gets its cb. */
			int seq = atomic_load(xal->seq_lock);

			if (!(seq & 1) && seq != cb_seq) {
				if (be->inotify->cb) {
					be->inotify->cb(xal, be->inotify->cb_args);
				}

				/* xal_index() advances seq_lock twice whether or not it worked,
				 * so on failure the advance is not a new version. Latch to where
				 * it ended up, so as to not fire again. */
				cb_seq = atomic_load(&xal->last_index_err) ? atomic_load(xal->seq_lock)
									  : seq;
				continue;
			}

			/* Already fired for this version; only a new event re-arms us, and it must
			 * be read through check_events() -- a failed index leaves an IN_IGNORED per
			 * watch behind, and a blind drain would re-arm on those. The poll is the
			 * wait; the fd is non-blocking. */
			if (poll(&pfd, 1, XAL_INOTIFY_POLL_TIMEOUT_MS) <= 0) {
				continue;
			}

			err = check_events(be->inotify);
			if (err < 0) {
				XAL_DEBUG("FAILED: check_events(); err(%d), exit thread", err);
				xal_mark_dirty(xal);
				goto exit_thread;
			}
			if (err == XAL_INOTIFY_REINDEX) {
				cb_seq = -1;
			}
			continue;
		}

		cb_seq = -1;

		/* The fd is non-blocking, so without this the read() below returns EAGAIN
		 * immediately and the loop becomes a busy poll. The timeout is what lets a
		 * dirty mark set elsewhere be noticed while no event ever arrives. */
		err = poll(&pfd, 1, XAL_INOTIFY_POLL_TIMEOUT_MS);
		if (err < 0) {
			if (errno == EINTR) {
				continue;
			}
			XAL_DEBUG("FAILED: poll(); errno(%d), exit thread", errno);
			err = -errno;
			xal_mark_dirty(xal);
			goto exit_thread;
		}
		if (!err) {
			continue;
		}

		err = check_events(be->inotify);
		if (err < 0) {
			XAL_DEBUG("FAILED: check_events(), exit thread; err(%d)", err);
			/* Nothing re-indexes once this loop is left, so leave the index dirty:
			 * readers get -ESTALE instead of extents for a vanished filesystem. */
			xal_mark_dirty(xal);
			goto exit_thread;
		}

		if (err) {
			XAL_DEBUG("INFO: Found breaking changes, marking xal as dirty");
			xal_mark_dirty(xal);
		}

	}

exit_thread:
	XAL_DEBUG("INFO: watch thread exiting");

	if (be->inotify) {
		atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_RUNNING);
	}
	pthread_exit((void *)(intptr_t)err);
}

int
xal_watch_filesystem(struct xal *xal, xal_dirty_cb cb, void *cb_args)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)&xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->inotify || !be->inotify->watch_mode) {
		XAL_DEBUG("FAILED: xal opened without watch mode");
		return -EINVAL;
	}

	/* Checked before the lock: neither has anything to do with the watch thread, and taking
	 * the lock only to fail would let a caller with a broken index block a reaper. */
	if (xal->root_idx == XAL_POOL_IDX_NONE) {
		XAL_DEBUG("FAILED: Missing call to xal_index()");
		return -EINVAL;
	}

	/* root_idx is claimed before the walk, so the check above catches an index that never ran
	 * but not one that ran and failed. */
	if (atomic_load(&xal->last_index_err)) {
		XAL_DEBUG("FAILED: the last index failed; err(%d)",
			  atomic_load(&xal->last_index_err));
		return atomic_load(&xal->last_index_err);
	}

	/* watch_thread_id and the flags are published together under this lock: a reaper sees no
	 * thread and no flags, or a written id and both flags, never one without the other. */
	pthread_mutex_lock(&be->inotify->lifecycle);

	if (atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_RUNNING) {
		pthread_mutex_unlock(&be->inotify->lifecycle);
		XAL_DEBUG("SKIPPED: thread already running");
		return 0;
	}

	/* Not running, so a JOINABLE thread here has already exited and this join returns at once;
	 * holding the lock across it costs nothing. */
	if (atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_JOINABLE) {
		pthread_join(be->inotify->watch_thread_id, NULL);
		atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_JOINABLE);
	}

	be->inotify->cb = cb;
	be->inotify->cb_args = cb_args;
	atomic_store(&be->inotify->stop, false);

	err = pthread_create(&be->inotify->watch_thread_id, NULL, &background_thread_start, xal);
	if (!err) {
		atomic_fetch_or(&be->inotify->flag,
				XAL_BE_FIEMAP_INOTIFY_RUNNING | XAL_BE_FIEMAP_INOTIFY_JOINABLE);
	}

	pthread_mutex_unlock(&be->inotify->lifecycle);

	if (err) {
		XAL_DEBUG("FAILED: pthread_create(); err(%d)", err);
		return -err;
	}

	return 0;
}

int
xal_stop_watching_filesystem(struct xal *xal)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	pthread_t tid;
	bool running;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)&xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->inotify || !be->inotify->watch_mode) {
		XAL_DEBUG("FAILED: xal opened without watch mode");
		return -EINVAL;
	}

	running = atomic_load(&be->inotify->flag) & XAL_BE_FIEMAP_INOTIFY_RUNNING;

	/* Reaped the same way whether it was told to stop or exited on its own; only the reported
	 * result differs, since stopping a watch that was not running is a caller error. */
	if (inotify_claim_join(be->inotify, true, &tid)) {
		err = pthread_join(tid, NULL);
		if (err) {
			XAL_DEBUG("FAILED: pthread_join(); err(%d)", err);
			return -err;
		}
		atomic_fetch_and(&be->inotify->flag, ~XAL_BE_FIEMAP_INOTIFY_RUNNING);
	}

	if (!running) {
		XAL_DEBUG("FAILED: thread is not running");
		return -EINVAL;
	}

	return 0;
}
