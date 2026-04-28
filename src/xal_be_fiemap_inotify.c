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

KHASH_MAP_INIT_INT64(wd_to_inode, struct xal_inode *);

int
xal_be_fiemap_process_inode_file(struct xal *xal, char *path, struct xal_inode *inode);

void
xal_be_fiemap_inotify_close(struct xal_inotify *inotify)
{
	kh_wd_to_inode_t *inode_map;

	if (!inotify) {
		XAL_DEBUG("SKIPPED: No xal_inotify given")
		return;
	}

	inode_map = inotify->inode_map;

	if (inode_map) {
		kh_destroy(wd_to_inode, inode_map);
	}

	if (inotify->fd) {
		close(inotify->fd);
	}

	if (inotify->flag & XAL_BE_FIEMAP_INOTIFY_RUNNING) {
		pthread_cancel(inotify->watch_thread_id);
	}
}

int
xal_be_fiemap_inotify_init(struct xal_inotify *inotify, enum xal_watchmode watch_mode)
{
	if (!inotify) {
		XAL_DEBUG("FAILED: No xal_inotify given");
		return -EINVAL;
	}

	inotify->watch_mode = watch_mode;

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
xal_be_fiemap_inotify_clear_inode_map(struct xal_inotify *inotify)
{
	khash_t(wd_to_inode) *inode_map;

	if (!inotify) {
		XAL_DEBUG("FAILED: No inotify object given");
		return -EINVAL;
	}

	inode_map = inotify->inode_map;

	kh_clear(wd_to_inode, inode_map);

	return 0;
}

int
xal_be_fiemap_inotify_add_watcher(struct xal_inotify *inotify, char *path, struct xal_inode *inode)
{
	khash_t(wd_to_inode) *inode_map;
	uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_UNMOUNT;
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

static int
check_events(struct xal *xal, struct xal_inotify *inotify)
{
	struct xal_inode *dir_inode, *inode;
	kh_wd_to_inode_t *inode_map;
	char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	char path[XAL_PATH_MAXLEN];
	khiter_t iter;
	ssize_t len, i;
	struct stat st;
	int err;

	inode_map = inotify->inode_map;

	len = read(inotify->fd, buf, sizeof buf);
	while (len > 0) {
		int wd;
		i = 0;

		while (i < len) {
			struct inotify_event *event = (struct inotify_event *)&buf[i];
			__attribute__((unused)) char mask_pp[128];

			inode = NULL;  // reset the pointer to the inode
			wd = event->wd;

			XAL_DEBUG_FCALL(inotify_event_mask_pp, event->mask, mask_pp, 128);
			XAL_DEBUG("INFO: mask(%s) for event with wd(%d) and name(%s)", &mask_pp[1], wd, event->name)

			if (inotify->watch_mode == XAL_WATCHMODE_DIRTY_DETECTION) {
				XAL_DEBUG("INFO: File system has changed;");
				return 1;
			}

			if (event->mask & IN_UNMOUNT) {
				XAL_DEBUG("FAILED: File system has been unmounted");
				return -EINVAL;
			}

			if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
				iter = kh_get(wd_to_inode, inode_map, wd);
				if (iter == kh_end(inode_map)) {
					XAL_DEBUG("FAILED: kh_get(%d) for event with name(%s)", wd, event->name);
					return -EINVAL;
				}

				XAL_DEBUG("INFO: found watch descriptor(%d) for event with name(%s)", wd, event->name);

				dir_inode = kh_val(inode_map, iter);
				if (!xal_inode_is_dir(dir_inode)) {
					XAL_DEBUG("FAILED: found inode(%s) is not a directory", dir_inode->name);
					return -EINVAL;
				}

				if (dir_inode->namelen + 1 + strlen(event->name) + 1 > sizeof(path)) {
					XAL_DEBUG("FAILED: event(%s) full path too long(%zu)",
							event->name, dir_inode->namelen + 1 + strlen(event->name) + 1);
					return -EINVAL;
				}
				memcpy(path, dir_inode->name, dir_inode->namelen);
				path[dir_inode->namelen] = '/';
				memcpy(path + dir_inode->namelen + 1, event->name, strlen(event->name));
				path[dir_inode->namelen + 1 + strlen(event->name)] = '\0';

				XAL_DEBUG("INFO: got full path of event: %s", path);
				atomic_fetch_add(&xal->seq_lock, 1);

				for (uint32_t j = 0; j < dir_inode->content.dentries.count; ++j) {
					struct xal_inode *child = xal_inode_at(xal, dir_inode->content.dentries.inodes_idx + j);

					if (strcmp(child->name, path) == 0) {
						inode = child;
						break;
					}
				}

				if (!inode) {
					XAL_DEBUG("FAILED: could not find child with name(%s)", event->name);
					err = -EINVAL;
					goto failed_with_lock;
				}

				XAL_DEBUG("INFO: reprocessing inode:");
				XAL_DEBUG_FCALL(xal_inode_pp, xal, inode);

				err = xal_be_fiemap_process_inode_file(xal, path, inode);
				if (err) {
					XAL_DEBUG("FAILED: xal_be_fiemap_process_inode_file(); err(%d)", err);
					goto failed_with_lock;
				}

				// Update to new file size
				err = stat(path, &st);
				if (err) {
					XAL_DEBUG("FAILED: stat(%s) errno(%d) while getting new file size", path, errno);
					err = -errno;
					goto failed_with_lock;
				}
				inode->size = st.st_size;

				XAL_DEBUG("INFO: finished reprocessing inode:");
				XAL_DEBUG_FCALL(xal_inode_pp, xal, inode);

				atomic_fetch_add(&xal->seq_lock, 1);

			} else if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVE)) {
				XAL_DEBUG("INFO: File system has changed, event mask:%s", mask_pp);
				return 1;
			}

			i += sizeof(struct inotify_event) + event->len;
		}

		len = read(inotify->fd, buf, sizeof buf);
	}

	return 0;

failed_with_lock:
	atomic_fetch_add(&xal->seq_lock, 1);

	return err;
}

static void *
background_thread_start(void *arg)
{
	struct xal *xal = arg;
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	int err = 0;

	XAL_DEBUG("INFO: starting background thread");

	if (!be->inotify) {
		XAL_DEBUG("FAILED: inotify not initialised, exit thread");
		goto exit_thread;
	}

	be->inotify->flag |= XAL_BE_FIEMAP_INOTIFY_RUNNING;

	err = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	if (err) {
		XAL_DEBUG("FAILED: pthread_setcancelstate(), exit thread; err(%d)", err);
		goto exit_thread;
	}

	while (1) {
		if (xal->dirty) {
			continue;
		}

		err = check_events(xal, be->inotify);
		if (err < 0) {
			XAL_DEBUG("FAILED: xal_be_fiemap_inotify_check_events(), exit thread; err(%d)", err);
			goto exit_thread;
		}

		if (err) {
			XAL_DEBUG("INFO: Found breaking changes, setting xal->dirty to true");
			atomic_store(&xal->dirty, true);
			if (be->inotify->cb) {
				be->inotify->cb(xal, be->inotify->cb_args);
			}
		}

	}

exit_thread:
	XAL_DEBUG("INFO: unlocked xal lock");

	be->inotify->flag &= ~XAL_BE_FIEMAP_INOTIFY_RUNNING;
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

	if (be->inotify->flag & XAL_BE_FIEMAP_INOTIFY_RUNNING) {
		XAL_DEBUG("SKIPPED: thread already running");
		return 0;
	}

	if (xal->root_idx == XAL_POOL_IDX_NONE) {
		XAL_DEBUG("FAILED: Missing call to xal_index()");
		return -EINVAL;
	}

	be->inotify->cb = cb;
	be->inotify->cb_args = cb_args;

	err = pthread_create(&be->inotify->watch_thread_id, NULL, &background_thread_start, xal);
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

	if (be->inotify->flag & ~XAL_BE_FIEMAP_INOTIFY_RUNNING) {
		XAL_DEBUG("FAILED: thread is not running");
		return -EINVAL;
	}

	pthread_cancel(be->inotify->watch_thread_id);
	err = pthread_cancel(be->inotify->watch_thread_id);
	if (err) {
		XAL_DEBUG("FAILED: pthread_cancel(); err(%d)", err);
		return -err;
	}

	be->inotify->flag &= ~XAL_BE_FIEMAP_INOTIFY_RUNNING;

	return 0;
}
