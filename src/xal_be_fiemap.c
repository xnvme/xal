#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_inotify.h>
#include <xal_odf.h>
#include <xal_bpf_events.h>
#include <xal_bpf.h>

KHASH_MAP_INIT_STR(path_to_inode, struct xal_inode *)

/** Basename prefix of the per-session reflink shadow directory: <mnt>/.xal_snapshot.<pid> */
#define XAL_SNAPSHOT_PREFIX ".xal_snapshot."

/** Max length of a "<dir>/<entry>" path assembled under the shadow directory. */
#define XAL_SNAPSHOT_ENTRY_MAXLEN (XAL_PATH_MAXLEN + 64)

/**
 * Reflink-snapshot state (XAL_WATCHMODE_REFLINK_SNAPSHOT).
 *
 * At index time every regular file the walk visits is reflink-cloned into a private shadow
 * directory (the walk is scoped by be->subtree, so the subtree restriction is applied there, not
 * here). The clone's on-disk inode keeps the shared blocks allocated for the whole xal session --
 * so the extents captured from it stay valid even as the origin is rewritten (writes to the origin
 * divert to new blocks via CoW). The clones are removed at xal_close().
 */
struct xal_reflink {
	char *dir;        ///< Shadow directory holding the clones: <mountpoint>/.xal_snapshot.<pid>
	bool dir_created; ///< The shadow directory is created lazily on the first clone
};

/**
 * Set or clear the immutable inode flag (FS_IMMUTABLE_FL) on an open fd. Requires
 * CAP_LINUX_IMMUTABLE; the fd need not be writable (immutable permits read-open).
 */
static int
reflink_chattr_immutable(int fd, bool enable)
{
	int attr;

	if (ioctl(fd, FS_IOC_GETFLAGS, &attr) < 0) {
		return -errno;
	}

	if (enable) {
		attr |= FS_IMMUTABLE_FL;
	} else {
		attr &= ~FS_IMMUTABLE_FL;
	}

	if (ioctl(fd, FS_IOC_SETFLAGS, &attr) < 0) {
		return -errno;
	}

	return 0;
}

/**
 * Remove the (flat) shadow directory and every clone within it. Clones are immutable, so the flag
 * is cleared before each unlink (may_delete refuses immutable inodes).
 */
static void
reflink_dir_purge(const char *dir)
{
	struct dirent *entry;
	DIR *d;

	d = opendir(dir);
	if (!d) {
		return;
	}

	while ((entry = readdir(d))) {
		char path[XAL_SNAPSHOT_ENTRY_MAXLEN]; // holds <dir>/<clone-name>
		int fd;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
			XAL_DEBUG("FAILED: clone path truncated under dir(%s); skipping", dir);
			continue;
		}

		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			int cerr = reflink_chattr_immutable(fd, false);

			if (cerr) {
				XAL_DEBUG("WARNING: could not clear immutable on clone(%s); err(%d); the "
					  "unlink below will likely fail", path, cerr);
			}
			close(fd);
		}
		if (unlink(path) && errno != ENOENT) {
			XAL_DEBUG("FAILED: unlink(%s); errno(%d); clone may be left behind", path,
				  errno);
		}
	}

	closedir(d);
	rmdir(dir);
}

/**
 * Remove every reflink shadow directory under @mountpoint, including those left by a crashed prior
 * run of any pid, so orphaned immutable clones do not accumulate. Assumes a single xal instance per
 * mount; a concurrent instance's live snapshot on the same mount would be swept too.
 */
static void
reflink_sweep_orphans(const char *mountpoint)
{
	struct dirent *entry;
	char own[64];
	DIR *d;

	d = opendir(mountpoint);
	if (!d) {
		return;
	}

	// This process's own shadow dir basename; a re-index sweeps it and that is expected.
	snprintf(own, sizeof(own), "%s%d", XAL_SNAPSHOT_PREFIX, (int)getpid());

	while ((entry = readdir(d))) {
		char dir[XAL_SNAPSHOT_ENTRY_MAXLEN];

		if (strncmp(entry->d_name, XAL_SNAPSHOT_PREFIX, sizeof(XAL_SNAPSHOT_PREFIX) - 1)) {
			continue;
		}
		if (snprintf(dir, sizeof(dir), "%s/%s", mountpoint, entry->d_name) >= (int)sizeof(dir)) {
			XAL_DEBUG("FAILED: shadow dir path truncated under mountpoint(%s); skipping",
				  mountpoint);
			continue;
		}

		// Any dir that is not our own is a genuine residual: a crashed prior run or a
		// concurrent live instance on this mount.
		if (strcmp(entry->d_name, own) != 0) {
			XAL_DEBUG("WARNING: residual shadow directory(%s); prior crash or another "
				  "live instance on this mount?", dir);
		}
		reflink_dir_purge(dir);
	}

	closedir(d);
}

/**
 * Lazily create a fresh shadow directory, clearing any stale leftover from a prior crash.
 */
static int
reflink_dir_prepare(struct xal_reflink *rl)
{
	reflink_dir_purge(rl->dir);

	if (mkdir(rl->dir, 0700)) {
		XAL_DEBUG("FAILED: mkdir(%s); errno(%d)", rl->dir, errno);
		return -errno;
	}
	rl->dir_created = true;

	return 0;
}

/**
 * Reflink-clone the file open at @origin_fd into the shadow directory.
 *
 * FICLONE itself flushes the origin's dirty pages (write-and-wait under the iolock) and resolves
 * delayed allocation to real blocks before sharing, so no explicit fsync is needed for the clone's
 * FIEMAP to see true physical extents. The clone is left on-disk (its inode pins the shared
 * blocks); the fd returned in @clone_fd is only needed to FIEMAP the clone and may be closed
 * afterwards without freeing blocks.
 *
 * One clone per inode instance: a hardlink to the same inode reuses the existing clone rather
 * than cloning the same data again. Clones are named "<ino>.<gen>" so a recycled inode number
 * (a different instance) never collides with an earlier one.
 */
static int
reflink_clone_file(struct xal_be_fiemap *be, const char *path, int origin_fd, int *clone_fd)
{
	struct xal_reflink *rl = be->reflink;
	char clone_path[XAL_SNAPSHOT_ENTRY_MAXLEN];
	struct stat sb;
	int cfd, err, gen = 0;

	if (!rl->dir_created) {
		err = reflink_dir_prepare(rl);
		if (err) {
			return err;
		}
	}

	// No explicit fsync: FICLONE (xfs_reflink_remap_prep -> __generic_remap_file_range_prep)
	// does filemap_write_and_wait_range on the source under the iolock, writing back the origin's
	// dirty pages and resolving delalloc to real blocks before sharing -- exactly what the clone's
	// FIEMAP needs. Device-flush durability is unneeded for the live raw-read model.

	// Name the clone "<ino>.<gen>". The inode number is the primary key ("find <mnt> -inum <ino>"
	// maps a clone back to its current origin); the generation number distinguishes inode
	// instances so a recycled inode -- freed and reallocated to a different file during the walk
	// -- gets a fresh clone instead of being mistaken for the earlier one.
	if (fstat(origin_fd, &sb)) {
		XAL_DEBUG("FAILED: fstat(origin); errno(%d)", errno);
		return -errno;
	}
	if (ioctl(origin_fd, FS_IOC_GETVERSION, &gen)) {
		XAL_DEBUG("FAILED: FS_IOC_GETVERSION(origin); errno(%d)", errno);
		return -errno;
	}

	if (snprintf(clone_path, sizeof(clone_path), "%s/%llu.%u", rl->dir,
		     (unsigned long long)sb.st_ino, (unsigned)gen) >= (int)sizeof(clone_path)) {
		XAL_DEBUG("FAILED: clone path truncated under dir(%s)", rl->dir);
		return -ENAMETOOLONG;
	}

	// One clone per inode instance. EEXIST means this (ino, gen) was already cloned this run --
	// a hardlink to the same inode -- so reuse the existing (already immutable) clone instead of
	// cloning the same data again.
	cfd = open(clone_path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (cfd < 0) {
		if (errno == EEXIST) {
			cfd = open(clone_path, O_RDONLY);
			if (cfd < 0) {
				XAL_DEBUG("FAILED: open(%s); errno(%d)", clone_path, errno);
				return -errno;
			}
			XAL_DEBUG("INFO: reused clone for file(%s)", path);
			*clone_fd = cfd;
			return 0;
		}
		XAL_DEBUG("FAILED: open(%s); errno(%d)", clone_path, errno);
		return -errno;
	}

	if (ioctl(cfd, FICLONE, origin_fd) < 0) {
		err = -errno;
		XAL_DEBUG("FAILED: FICLONE(); errno(%d)%s", errno,
			  errno == EOPNOTSUPP ? " (filesystem lacks reflink)" : "");
		goto fail_created;
	}

	// Harden the clone: immutable makes the kernel refuse defrag (swapext/exchange-range) on it,
	// so its physical extents cannot be relocated for the life of the snapshot. Immutable also
	// blocks unlink/rm even as root, so xal clears it before unlinking (reflink_dir_purge); manual
	// cleanup of an orphaned shadow dir needs: chattr -R -i <mnt>/.xal_snapshot.* && rm -rf <same>.
	err = reflink_chattr_immutable(cfd, true);
	if (err) {
		XAL_DEBUG("FAILED: set immutable on clone(%s); err(%d)%s", clone_path, err,
			  err == -EPERM ? " (need CAP_LINUX_IMMUTABLE)" : "");
		goto fail_created;
	}

	XAL_DEBUG("INFO: reflinked file(%s)", path);
	*clone_fd = cfd;

	return 0;

fail_created:
	close(cfd);
	unlink(clone_path); // we created it with O_EXCL, so it is ours to remove
	return err;
}

static int
process_ino_fiemap(struct xal *xal, char *path, struct xal_inode *self);

static int
xal_be_fiemap_index(struct xal *xal);

void
xal_be_fiemap_close(struct xal *xal)
{
	struct xal_be_fiemap *be;
	kh_path_to_inode_t *inode_map;

	if (!xal) {
		return;
	}

	be = (struct xal_be_fiemap *)&xal->be;

	if (be->inotify) {
		xal_be_fiemap_inotify_close(be->inotify);
	} else if (be->reflink) {
		if (be->reflink->dir_created) {
			reflink_dir_purge(be->reflink->dir);
		}
		free(be->reflink->dir);
		free(be->reflink);
	} else {
#ifdef XAL_BPF_ENABLED
		if (be->bpf) {
			xal_be_fiemap_bpf_close(be->bpf);
		}
#endif /* XAL_BPF_ENABLED */

		int fd = open(be->mountpoint, O_RDONLY | O_DIRECTORY);

		if (fd >= 0) {
			int err = ioctl(fd, FITHAW, 0);
			if (err == 0) {
				XAL_DEBUG("INFO: thawed filesystem");
			} else if (errno == EINVAL) {
				XAL_DEBUG("INFO: FITHAW returned EINVAL; already thawed?");
			} else {
				XAL_DEBUG("ERROR: could not thaw filesystem; errno(%d)", errno);
			}
			close(fd);
		} else {
			XAL_DEBUG("FAILED: could not open() fs mountpoint for thaw");
		}
	}

	free(be->mountpoint);
	free(be->subtree);

	inode_map = be->path_inode_map;

	if (be->path_inode_map) {
		kh_destroy(path_to_inode, inode_map);
	}

	return;
}

static bool
_is_directory_member(char *name)
{
	bool is_self = strcmp(name, ".") == 0;
	bool is_parent = strcmp(name, "..") == 0;
	return !is_self && !is_parent;
}

static int
retrieve_total_entries(char *path)
{
	struct stat sb;
	struct dirent *entry;
	DIR *d;
	int count, err;

	err = stat(path, &sb);
	if (err) {
		if (errno == ENOENT) {
			XAL_DEBUG("FAILED: stat(%s); No such file or directory, try again", path);
			return -EAGAIN;
		}
		XAL_DEBUG("FAILED: stat(%s); errno(%d)", path, errno);
		return -errno;
	}

	if (!S_ISDIR(sb.st_mode)) {
		XAL_DEBUG("INFO: path(%s) is not a directory", path);
		return 0;
	}

	d = opendir(path);
	if (!d) {
		XAL_DEBUG("FAILED: opendir(); errno(%d)", errno);
		return -errno;
	}

	count = 0;
	entry = readdir(d);
	while (entry) {
		if (!_is_directory_member(entry->d_name)) {
			entry = readdir(d);
			continue;
		}

		count += 1;
		if (entry->d_type == DT_DIR) {
			char subpath[strlen(path) + 1 + strlen(entry->d_name) + 1];
			int children;

			snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
			children = retrieve_total_entries(subpath);

			if (children < 0) {
				return -1;
			}

			count += children;
		}
		entry = readdir(d);
	}
	closedir(d);

	return count;
}

int
xal_be_fiemap_open(struct xal **xal, char *mountpoint, struct xal_opts *opts)
{
	struct xal *cand;
	struct stat sb;
	struct xal_be_fiemap *be;
	char shm_name[XAL_PATH_MAXLEN + 9];
	const char *shm;
	int nallocated, err;

	if (!mountpoint) {
		XAL_DEBUG("FAILED: No mountpoint given");
		return -EINVAL;
	}

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		XAL_DEBUG("FAILED: calloc(); errno(%d)", errno);
		return -errno;
	}

	cand->root_idx = XAL_POOL_IDX_NONE;
	cand->index_state = &cand->_index_state_storage;
	cand->seq_lock = &cand->_seq_lock_storage;

	be = (struct xal_be_fiemap *)&cand->be;

	be->base.type = XAL_BACKEND_FIEMAP;
	be->base.close = xal_be_fiemap_close;
	be->base.index = xal_be_fiemap_index;

	be->mountpoint = calloc(strlen(mountpoint) + 1, sizeof(char));
	if (!be->mountpoint) {
		XAL_DEBUG("FAILED: calloc(); errno(%d)", errno);
		err = -errno;
		goto failed;
	}

	strcpy(be->mountpoint, mountpoint);
	err = stat(be->mountpoint, &sb);
	if (err) {
		XAL_DEBUG("FAILED: stat(%s); errno(%d)", be->mountpoint, errno);
		err = -errno;
		goto failed;
	}

	// Optional subtree: scope the index walk to a path at/under the mountpoint. General to the
	// FIEMAP backend (any watch mode); in reflink-snapshot mode it also bounds what gets cloned,
	// since only the walked files are reflinked.
	if (opts->subtree && strlen(opts->subtree)) {
		size_t mplen = strlen(mountpoint);
		struct stat st;

		be->subtree = strdup(opts->subtree);
		if (!be->subtree) {
			XAL_DEBUG("FAILED: strdup(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		// Matched against absolute, mountpoint-rooted paths, so it must be an absolute path at or
		// under the mountpoint. Reject a malformed one (relative, typo, wrong mount) rather than
		// silently indexing nothing.
		if (strncmp(be->subtree, mountpoint, mplen) != 0 ||
		    (be->subtree[mplen] != '\0' && be->subtree[mplen] != '/')) {
			XAL_DEBUG("FAILED: subtree(%s) is not under mountpoint(%s)", be->subtree,
				  mountpoint);
			err = -EINVAL;
			goto failed;
		}

		// Require the subtree to exist and be a directory now, so a typo'd or missing path is
		// rejected here with a clear error instead of surfacing later during the walk.
		if (stat(be->subtree, &st) != 0) {
			XAL_DEBUG("FAILED: stat(subtree=%s); errno(%d)", be->subtree, errno);
			err = -errno;
			goto failed;
		}
		if (!S_ISDIR(st.st_mode)) {
			XAL_DEBUG("FAILED: subtree(%s) is not a directory", be->subtree);
			err = -ENOTDIR;
			goto failed;
		}
	}

	if (opts->watch_mode == XAL_WATCHMODE_REFLINK_SNAPSHOT) {
		// reflink-snapshot mode: no inotify watch, no freeze; clones pin the blocks
		size_t dlen;

		be->reflink = calloc(1, sizeof(struct xal_reflink));
		if (!be->reflink) {
			XAL_DEBUG("FAILED: calloc(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		dlen = strlen(mountpoint) + 32;
		be->reflink->dir = malloc(dlen);
		if (!be->reflink->dir) {
			XAL_DEBUG("FAILED: malloc(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}
		snprintf(be->reflink->dir, dlen, "%s/" XAL_SNAPSHOT_PREFIX "%d", mountpoint,
			 (int)getpid());

		// Sweep pre-existing shadow dirs now so orphans are cleaned even if the caller never
		// indexes; xal_index() sweeps again (and resets dir_created) before each (re)snapshot.
		reflink_sweep_orphans(mountpoint);

		XAL_DEBUG("INFO: reflink-snapshot mode; clones under dir(%s), subtree(%s)",
			  be->reflink->dir, be->subtree ? be->subtree : "(whole tree)");
	} else if (opts->watch_mode) {
		be->inotify = calloc(1, sizeof(struct xal_inotify));
		if (!be->inotify) {
			XAL_DEBUG("FAILED: calloc(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		err = xal_be_fiemap_inotify_init(be->inotify, opts->watch_mode);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_inotify_init()");
			goto failed;
		}
	} else {
#ifdef XAL_BPF_ENABLED
		// since fs is mounted without a watch mode, freeze it
		// + init bpf thread to listen to unfreeze events
		struct xal_bpf *bpf = calloc(1, sizeof(struct xal_bpf));
		if (!bpf) {
			XAL_DEBUG("FAILED: calloc(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		// glibc major()/minor() and the kernel's MKDEV(20,12) split on s_dev
		// produce the same numeric values, so userspace and BPF can compare
		// directly. If a kernel changes that split, the BPF filter will start
		// dropping every event as "ignored" -- check skel->bss->stats.ignored_events.
		bpf->ctx.dev_major = major(sb.st_dev);
		bpf->ctx.dev_minor = minor(sb.st_dev);
		bpf->ctx.fs_block_size = sb.st_blksize;

		err = xal_be_fiemap_bpf_init(bpf);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_bpf_init()");
			goto failed;
		}

		be->bpf = bpf;
#endif /* XAL_BPF_ENABLED */

		int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

		if (fd < 0) {
			XAL_DEBUG("FAILED: open(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		// when ioctl returns, fs is fully frozen
		err = ioctl(fd, FIFREEZE, 0);
		if (err == 0) {
			XAL_DEBUG("INFO: froze filesystem");
		} else if (errno == EBUSY) {
			XAL_DEBUG("INFO: FIFREEZE returned EBUSY; already frozen?");
		} else {
			close(fd);
			XAL_DEBUG("FAILED: could not freeze filesystem; errno(%d)", errno);
			goto failed;
		}
		close(fd);

#ifdef XAL_BPF_ENABLED
		err = xal_be_fiemap_bpf_rb_init(cand, be->bpf);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_bpf_rb_init(); err(%d)", err);
			goto failed;
		}

		err = xal_bpf_start_poll_thread(cand);
		if (err) {
			XAL_DEBUG("FAILED: xal_bpf_start_poll_thread(); err(%d)", err);
			goto failed;
		}
#endif /* XAL_BPF_ENABLED */
	}

	// Scope the pre-count to the subtree when set: the index walks only that subtree
	// (see xal_be_fiemap_index), so counting from the mountpoint would over-reserve the inode pool.
	nallocated = retrieve_total_entries(be->subtree ? be->subtree : be->mountpoint);
	if (nallocated < 0) {
		XAL_DEBUG("Failed: retrieve_total_entries()");
		err = nallocated;
		goto failed;
	}

	cand->sb.blocksize = sb.st_blksize;
	cand->sb.rootino = sb.st_ino;

	if (opts->shm_name && strlen(opts->shm_name) > XAL_PATH_MAXLEN) {
		XAL_DEBUG("FAILED: shm_name too long");
		err = -EINVAL;
		goto failed;
	}

	shm = NULL;

	if (opts->shm_name) {
		snprintf(shm_name, sizeof(shm_name), "%s_inodes", opts->shm_name);
		shm = shm_name;
	}
	err = xal_pool_map(&cand->inodes, 40000000UL, nallocated, sizeof(struct xal_inode), shm);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(inodes); err(%d)", err);
		goto failed;
	}

	shm = NULL;
	if (opts->shm_name) {
		snprintf(shm_name, sizeof(shm_name), "%s_extents", opts->shm_name);
		shm = shm_name;
	}
	err = xal_pool_map(&cand->extents, 40000000UL, nallocated, sizeof(struct xal_extent), shm);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_map(extents); err(%d)", err);
		goto failed;
	}

	if (opts->file_lookupmode == XAL_FILE_LOOKUPMODE_HASHMAP) {
		be->path_inode_map = kh_init(path_to_inode);
		if (!be->path_inode_map) {
			XAL_DEBUG("FAILED: kh_init()");
			err = -EINVAL;
			goto failed;
		}
	}

	*xal = cand; // All is good; promote the candidate

	return 0;

failed:
	xal_close(cand);

	return err;
}

static int
compare_dirent(const void *a, const void *b)
{
	const char *da = *(const char **)a;
	const char *db = *(const char **)b;
	return strcmp(da, db);
}

static int
xal_be_fiemap_process_inode_dir(struct xal *xal, char *path, struct xal_inode *inode)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct dirent *entry;
	char **entries = NULL;
	DIR *d;
	size_t n_entries = 0, capacity = 0;
	int err;

	if (!xal_inode_is_dir(inode)) {
		XAL_DEBUG("FAILED: cannot process directory at path(%s) - not a directory", path);
		return -EINVAL;
	}

	if (be->inotify) {
		err = xal_be_fiemap_inotify_add_watcher(be->inotify, path, inode);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_inotify_add_watcher(); err(%d)", err);
			return err;
		}
	}

	/* Count number of directory entried, no processing yet */
	d = opendir(path);
	if (!d) {
		XAL_DEBUG("FAILED: opendir(); errno(%d)", errno);
		return -errno;
	}

	entry = readdir(d);
	while (entry) {
		char *name;

		if (!_is_directory_member(entry->d_name)) {
			entry = readdir(d);
			continue;
		}

		// Never index our own reflink shadow dirs (defensive: they are purged before the walk,
		// but skip any that are present so a re-index does not descend in and clone the clones).
		if (be->reflink &&
		    strncmp(entry->d_name, XAL_SNAPSHOT_PREFIX, sizeof(XAL_SNAPSHOT_PREFIX) - 1) == 0) {
			entry = readdir(d);
			continue;
		}

		if (n_entries == capacity) {
			char **tmp;

			capacity = capacity ? capacity * 2 : 64;

			tmp = realloc(entries, capacity * sizeof(*entries));
			if (!tmp) {
				XAL_DEBUG("FAILED: realloc(); errno(%d)", errno);
				err = -ENOMEM;
				goto exit;
			}
			entries = tmp;
		}

		name = strdup(entry->d_name);
		if (!name) {
			XAL_DEBUG("FAILED: strdup(); errno(%d)", errno);
			err = -ENOMEM;
			goto exit;
		}

		entries[n_entries++] = name;
		entry = readdir(d);
	}

	qsort(entries, n_entries, sizeof(*entries), compare_dirent);

	err = xal_pool_claim_inodes(&xal->inodes, n_entries, &inode->content.dentries.inodes_idx);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_inodes(); err(%d)", err);
		goto exit;
	}
	inode->content.dentries.count = 0;

	/* Actually process directory entries */
	for (size_t i = 0; i < n_entries; i++) {
		char *entry_name = entries[i];

		struct xal_inode *dentry = xal_inode_at(xal, inode->content.dentries.inodes_idx + inode->content.dentries.count);

		char dentry_path[strlen(path) + 1 + strlen(entry_name) + 1];
		snprintf(dentry_path, sizeof(dentry_path), "%s/%s", path, entry_name);

		strcpy(dentry->name, dentry_path);
		dentry->namelen = strlen(dentry->name);
		dentry->parent_idx = xal_inode_idx(xal, inode);

		inode->content.dentries.count += 1;

		err = process_ino_fiemap(xal, dentry_path, dentry);
		if (err) {
			XAL_DEBUG("FAILED: process_ino_fiemap(); with path(%s)", dentry_path);
			goto exit;
		}
	}

	if (be->path_inode_map) {
		khash_t(path_to_inode) *map = be->path_inode_map;
		khiter_t iter;

		iter = kh_put(path_to_inode, map, inode->name, &err);
		if (err < 0) {
			XAL_DEBUG("FAILED: kh_put(); err(%d)", err);
			err = -EIO;
			goto exit;
		}

		kh_value(map, iter) = inode;
		err = 0;
	}

exit:
	closedir(d);

	for (size_t i = 0; i < n_entries; i++) {
		free(entries[i]);
	}
	free(entries);

	return err;
}

/*
 * Take a pointer to a fiemap struct with an fm_extents array of size 0.
 * The ioctl sets the "mapped_extents" integer to the amount of extents
 * existing in the file descriptor, so we reallocate the fiemap to be of
 * the right size, and then run the ioctl again with "fm_extent_count"
 * set to the right size too, such that all the extents are read into the
 * struct.
 */
static int
read_fiemap(int fd, struct fiemap **fiemap_ptr)
{
	struct fiemap *fiemap = *fiemap_ptr;
	int extents_size;

	if (!fiemap) {
		return -EINVAL;
	}

	fiemap->fm_length = ~0;  // maximum number of bits
	fiemap->fm_extent_count = 0;  // read 0 extents

	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		XAL_DEBUG("FAILED: fiemap ioctl(); errno(%d)", errno);
		return -errno;
	}

	extents_size = sizeof(struct fiemap_extent) * fiemap->fm_mapped_extents;

	fiemap = realloc(fiemap, sizeof(struct fiemap) + extents_size);
	if (!fiemap) {
		XAL_DEBUG("FAILED: fiemap realloc(); errno(%d)", errno);
		return -errno;
	}

	memset(fiemap->fm_extents, 0, extents_size);
	fiemap->fm_extent_count = fiemap->fm_mapped_extents;
	fiemap->fm_mapped_extents = 0;

	// TODO: writeback could happen between the first and second ioctl.
	// check that fm_extent_count == fm_mapped_extents. retry if otherwise.
	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		XAL_DEBUG("FAILED: fiemap ioctl(); errno(%d)", errno);
		return -errno;
	}

	*fiemap_ptr = fiemap;
	return 0;
}

int
xal_be_fiemap_process_inode_file(struct xal *xal, char *path, struct xal_inode *inode)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct fiemap *fiemap = NULL;
	int fd, map_fd, clone_fd = -1, err = 0;

	if (!xal_inode_is_file(inode)) {
		XAL_DEBUG("FAILED: cannot process file at path(%s) - not a file", path);
		return -EINVAL;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		XAL_DEBUG("FAILED: open(%s); errno(%d)", path, errno);
		return -errno;
	}

	// In reflink-snapshot mode, capture extents from a reflink clone (whose blocks are pinned
	// for the session) instead of the live origin, so they stay valid under concurrent writes.
	map_fd = fd;
	if (be->reflink) {
		err = reflink_clone_file(be, path, fd, &clone_fd);
		if (err) {
			XAL_DEBUG("FAILED: reflink_clone_file(path=%s); err(%d)", path, err);
			goto failed;
		}
		map_fd = clone_fd;
	}

	fiemap = malloc(sizeof(struct fiemap));
	if (!fiemap) {
		XAL_DEBUG("FAILED: malloc(); errno(%d)", errno);
		err = -ENOMEM;
		goto failed;
	}
	memset(fiemap, 0, sizeof(struct fiemap));

	err = read_fiemap(map_fd, &fiemap);
	if (err) {
		XAL_DEBUG("FAILED: read_fiemap(); err(%d)", err);
		goto failed;
	}

	if (fiemap->fm_mapped_extents > 0) {
		struct xal_extents *extents;

		err = xal_pool_claim_extents(&xal->extents, fiemap->fm_mapped_extents, &inode->content.extents.extent_idx);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_claim_extents(); err(%d)", err);
			goto failed;
		}

		extents = &inode->content.extents;
		extents->count = fiemap->fm_mapped_extents;

		for (uint32_t i = 0; i < extents->count; i++) {
			struct xal_extent *extent = xal_extent_at(xal, extents->extent_idx + i);

			extent->start_offset = fiemap->fm_extents[i].fe_logical / xal->sb.blocksize;
			extent->start_block  = fiemap->fm_extents[i].fe_physical / xal->sb.blocksize;
			extent->nblocks      = fiemap->fm_extents[i].fe_length / xal->sb.blocksize;
			extent->flag         = fiemap->fm_extents[i].fe_flags;
		}
	}

	free(fiemap);
	if (clone_fd >= 0) {
		close(clone_fd); // clone stays on-disk in the shadow dir; blocks remain pinned
	}
	close(fd);

	if (be->path_inode_map) {
		khash_t(path_to_inode) *map = be->path_inode_map;
		khiter_t iter;

		iter = kh_put(path_to_inode, map, inode->name, &err);
		if (err < 0) {
			XAL_DEBUG("FAILED: kh_put(); err(%d)", err);
			return -EIO;
		}
		kh_value(map, iter) = inode;
	}

	return 0;

failed:
	free(fiemap);
	if (clone_fd >= 0) {
		close(clone_fd);
	}
	if (fd >= 0) {
		close(fd);
	}

	return err;
}

static int
process_ino_fiemap(struct xal *xal, char *path, struct xal_inode *self)
{
	struct stat sb;
	int err;

	if (!path) {
		return -EINVAL;
	}

	err = stat(path, &sb);
	if (err) {
		if (errno == ENOENT) {
			XAL_DEBUG("FAILED: stat(%s); No such file or directory, try again", path);
			return -EAGAIN;
		}
		XAL_DEBUG("FAILED: stat(%s); errno(%d)", path, errno);
		return -errno;
	}

	if (!self->ftype) {
		if S_ISDIR(sb.st_mode) {
			self->ftype = XAL_ODF_DIR3_FT_DIR;
		} else if (S_ISREG(sb.st_mode)) {
			self->ftype = XAL_ODF_DIR3_FT_REG_FILE;
		} else {
			XAL_DEBUG("FAILED: unsupported ftype");
			return -EINVAL;
		}
	}

	self->ino = sb.st_ino;
	self->size = sb.st_size;

	switch(self->ftype) {
		case XAL_ODF_DIR3_FT_DIR:
			err = xal_be_fiemap_process_inode_dir(xal, path, self);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_fiemap_process_inode_dir(); err(%d)", err);
				return err;
			}
			break;
		case XAL_ODF_DIR3_FT_REG_FILE:
			err = xal_be_fiemap_process_inode_file(xal, path, self);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_fiemap_process_inode_file(); err(%d)", err);
				return err;
			}
			break;
		default:
			XAL_DEBUG("FAILED: unsupported ftype");
			return -ENOSYS;
	}

	return 0;
}

int
xal_be_fiemap_index(struct xal *xal)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct xal_inode *root;
	int err;

	if (!strlen(be->mountpoint)) {
		XAL_DEBUG("FAILED: xal object has no mountpoint");
		return -EINVAL;
	}

	atomic_store(xal->index_state, XAL_STATE_INDEXING);

	XAL_DEBUG("INFO: waiting for xal lock");
	atomic_fetch_add(xal->seq_lock, 1);

	xal_pool_clear(&xal->inodes);
	xal_pool_clear(&xal->extents);

	if (be->inotify) {
		err = xal_be_fiemap_inotify_drain(be->inotify);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_inotify_drain(); err(%d)", err);
			goto exit;
		}

		err = xal_be_fiemap_inotify_clear_inode_map(be->inotify);
		if (err) {
			XAL_DEBUG("FAILED: xal_be_fiemap_inotify_clear_inode_map(); err(%d)", err);
			goto exit;
		}
	}

	err = xal_pool_claim_inodes(&xal->inodes, 1, &xal->root_idx);
	if (err) {
		XAL_DEBUG("FAILED: xal_pool_claim_inodes(); err(%d)", err);
		goto exit;
	}

	root = xal_inode_at(xal, xal->root_idx);
	root->ino = xal->sb.rootino;
	root->ftype = XAL_ODF_DIR3_FT_DIR;
	root->namelen = 0;
	root->parent_idx = XAL_POOL_IDX_NONE;
	root->content.extents.count = 0;
	root->content.dentries.count = 0;

	// In reflink mode, sweep every shadow dir under the mountpoint before the walk -- orphans left
	// by a crashed prior run (any pid) and this handle's own previous index -- then reset so the
	// dir is rebuilt fresh. This cleans orphans, makes a re-index re-snapshot the current tree, and
	// keeps the shadow dir absent while the mountpoint is enumerated (so the walk cannot descend
	// into it and clone the clones).
	if (be->reflink) {
		reflink_sweep_orphans(be->mountpoint);
		be->reflink->dir_created = false;
	}

	// Scope the walk to the subtree when set: only files under it are indexed, so there is no
	// reason to traverse anything outside it. Rerooting the tree at the subtree keeps absolute-path
	// lookups working -- xal_be_fiemap_get_inode() strips the same subtree prefix as its basepath
	// (see there).
	char *walk_root = be->subtree ? be->subtree : be->mountpoint;

	err = process_ino_fiemap(xal, walk_root, root);
	if (err) {
		XAL_DEBUG("FAILED: process_ino_fiemap(); err(%d)", err);
		goto exit;
	}

	if (be->reflink) {
		XAL_DEBUG("INFO: reflink snapshot complete; clones under dir(%s)", be->reflink->dir);
	}

exit:
	atomic_fetch_add(xal->seq_lock, 1);
	xal_mark_index_done(xal, err);

	return err;
}

static int
build_hashmap_walk(struct xal *xal, struct xal_inode *inode)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	khash_t(path_to_inode) *map = be->path_inode_map;
	khiter_t iter;
	int err;

	if (inode->namelen > 0) {
		iter = kh_put(path_to_inode, map, inode->name, &err);
		if (err < 0) {
			XAL_DEBUG("FAILED: kh_put(%s); err(%d)", inode->name, err);
			return -EIO;
		}
		kh_value(map, iter) = inode;
	}

	if (xal_inode_is_dir(inode)) {
		for (uint32_t i = 0; i < inode->content.dentries.count; i++) {
			struct xal_inode *child = xal_inode_at(xal, inode->content.dentries.inodes_idx + i);

			err = build_hashmap_walk(xal, child);
			if (err) {
				return err;
			}
		}
	}

	return 0;
}

int
xal_build_lookup_hashmap(struct xal *xal)
{
	struct xal_be_fiemap *be;
	int err;

	if (!xal) {
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)&xal->be;

	if (be->base.type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: xal not opened with backend FIEMAP");
		return -EINVAL;
	}

	if (xal_is_dirty(xal)) {
		XAL_DEBUG("FAILED: File system has changed");
		return -ESTALE;
	}

	if (be->path_inode_map) {
		kh_destroy(path_to_inode, be->path_inode_map);
	}

	be->path_inode_map = kh_init(path_to_inode);
	if (!be->path_inode_map) {
		XAL_DEBUG("FAILED: kh_init()");
		return -ENOMEM;
	}

	err = build_hashmap_walk(xal, xal_inode_at(xal, xal->root_idx));
	if (err) {
		XAL_DEBUG("FAILED: build_hashmap_walk(); err(%d)", err);
		kh_destroy(path_to_inode, be->path_inode_map);
		be->path_inode_map = NULL;
		return err;
	}

	return 0;
}

int
xal_be_fiemap_get_inode(struct xal *xal, char *path, struct xal_inode **inode)
{
	struct xal_be_fiemap *be;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	if (!path) {
		XAL_DEBUG("FAILED: no path given");
		return -EINVAL;
	}

	be = (struct xal_be_fiemap *)&xal->be;

	if (be->base.type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: xal not opened with backend FIEMAP; be(%d)", be->base.type);
		return -EINVAL;
	}

	if (be->path_inode_map) {
		kh_path_to_inode_t *map = be->path_inode_map;
		khiter_t iter = kh_get(path_to_inode, map, path);

		if (iter == kh_end(map)) {
			XAL_DEBUG("FAILED: kh_get(%s)", path);
			return -EINVAL;
		}

		*inode = kh_val(map, iter);

	} else {
		// Match the basepath to the indexed tree root: when a subtree is set the walk is rerooted
		// at it, so strip the subtree prefix (not the mountpoint) from the query.
		char *basepath = be->subtree ? be->subtree : be->mountpoint;

		err = search_by_traversal(xal, xal_inode_at(xal, xal->root_idx), path, basepath, inode);
		if (err) {
			XAL_DEBUG("FAILED: search_by_traversal(%s); err(%d)", path, err);
			return err;
		}
	}

	return 0;
}
