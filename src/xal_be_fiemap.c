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
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_inotify.h>
#include <xal_odf.h>

KHASH_MAP_INIT_STR(path_to_inode, struct xal_inode *)

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

	be = (struct xal_be_fiemap *)xal->be;

	if (be->inotify) {
		xal_be_fiemap_inotify_close(be->inotify);
	} else {
		int fd = open(be->mountpoint, O_RDONLY | O_DIRECTORY);

		if (fd >= 0) {
			if(ioctl(fd, FITHAW, 0) < 0) {
				XAL_DEBUG("INFO: could not thaw filesystem; maybe nothing to thaw?");
			} else {
				XAL_DEBUG("INFO: thawed filesystem");
			}
			close(fd);
		} else {
			XAL_DEBUG("FAILED: could not open() fs mountpoint for thaw");
		}
	}

	free(be->mountpoint);

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
	cand->dirty = &cand->_dirty_storage;

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

	if (opts->watch_mode) {
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
		// since fs is mounted without a watch mode, freeze it
		int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

		if (fd < 0) {
			XAL_DEBUG("FAILED: open(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		// when ioctl returns, fs is fully frozen
		err = ioctl(fd, FIFREEZE, 0);
		if (err < 0) {
			close(fd);
			XAL_DEBUG("FAILED: could not freeze filesystem; err(%d)", err);
			goto failed;
		}
		close(fd);
		XAL_DEBUG("INFO: freezed filesystem");
	}

	nallocated = retrieve_total_entries(be->mountpoint);
	if (nallocated < 0) {
		XAL_DEBUG("Failed: retrieve_total_entries()");
		err = nallocated;
		goto failed;
	}

	err = stat(be->mountpoint, &sb);
	if (err) {
		XAL_DEBUG("FAILED: stat(%s); errno(%d)", be->mountpoint, errno);
		err = -errno;
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
	struct fiemap *fiemap;
	int fd, err = 0;

	if (!xal_inode_is_file(inode)) {
		XAL_DEBUG("FAILED: cannot process file at path(%s) - not a file", path);
		return -EINVAL;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		XAL_DEBUG("FAILED: open(%s); errno(%d)", path, errno);
		return -errno;
	}

	fiemap = malloc(sizeof(struct fiemap));
	if (!fiemap) {
		XAL_DEBUG("FAILED: malloc(); errno(%d)", errno);
		goto failed;
	}
	memset(fiemap, 0, sizeof(struct fiemap));

	err = read_fiemap(fd, &fiemap);
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
	if (fd) {
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

	XAL_DEBUG("INFO: waiting for xal lock");
	atomic_fetch_add(&xal->seq_lock, 1);

	xal_pool_clear(&xal->inodes);
	xal_pool_clear(&xal->extents);

	if (be->inotify) {
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

	err = process_ino_fiemap(xal, be->mountpoint, root);
	if (err) {
		XAL_DEBUG("FAILED: process_ino_fiemap(); err(%d)", err);
		goto exit;
	}

	atomic_store(xal->dirty, false);

exit:
	atomic_fetch_add(&xal->seq_lock, 1);

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
		err = search_by_traversal(xal, xal_inode_at(xal, xal->root_idx), path, be->mountpoint, inode);
		if (err) {
			XAL_DEBUG("FAILED: search_by_traversal(%s); err(%d)", path, err);
			return err;
		}
	}

	return 0;
}
