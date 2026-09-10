#include <asm-generic/errno.h>
#include <libxnvme.h>
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_xfs.h>
#include <xal_odf.h>
#include <xal_pp.h>

/**
 * Calculate the on-disk offset of the given filesystem block number
 *
 * Format Assumption
 * =================
 * |       agno        |       bno        |
 * | 64 - agblklog     |  agblklog        |
 */
uint64_t
xal_fsbno_offset(struct xal *xal, uint64_t fsbno)
{
	struct xal_backend_base *be = (struct xal_backend_base *)&xal->be;

	switch (be->type) {
		case XAL_BACKEND_FIEMAP:
			return fsbno * xal->sb.blocksize;

		case XAL_BACKEND_XFS:
			uint64_t ag, bno;

			ag = fsbno >> xal->sb.agblklog;
			bno = fsbno & ((1 << xal->sb.agblklog) - 1);

			return (ag * xal->sb.agblocks + bno) * xal->sb.blocksize;

		default:
			XAL_DEBUG("FAILED: Unknown backend type(%d)", be->type);
			return -EINVAL;
	}
}

struct xal_inode *
xal_inode_at(struct xal *xal, uint32_t idx)
{
	return (struct xal_inode *)xal->inodes.memory + idx;
}

struct xal_extent *
xal_extent_at(struct xal *xal, uint32_t idx)
{
	return (struct xal_extent *)xal->extents.memory + idx;
}

uint32_t
xal_inode_idx(struct xal *xal, struct xal_inode *inode)
{
	return (uint32_t)(inode - (struct xal_inode *)xal->inodes.memory);
}

void
xal_close(struct xal *xal)
{
	struct xal_backend_base *be;
	bool should_unlink;

	if (!xal) {
		return;
	}

	if (xal->procrole == XAL_PROCROLE_PRIMARY && xal->state) {
		xal_mark_dirty(xal);
	}

	be = (struct xal_backend_base *)&xal->be;
	if (be->close) {
		be->close(xal);
	}

	should_unlink = xal->procrole != XAL_PROCROLE_SECONDARY;

	xal_pool_unmap(&xal->inodes, should_unlink);
	xal_pool_unmap(&xal->extents, should_unlink);

	if (xal->state) {
		if (xal->state_shm_name) {
			shm_unlink(xal->state_shm_name);
			free(xal->state_shm_name);
		}
		munmap(xal->state, sizeof(struct xal_shared_state));
	}

	free(xal);
}

static int
retrieve_mountpoint(const char *dev_uri, char *mntpnt)
{
	FILE *f;
	char d[XAL_PATH_MAXLEN + 1], m[XAL_PATH_MAXLEN + 1];
	bool found = false;

	f = fopen("/proc/mounts", "r");
	if (!f) {
		XAL_DEBUG("FAILED: could not open /proc/mounts; errno(%d)", errno);
		return -errno;
	}

	while (fscanf(f, "%s %s%*[^\n]\n", d, m) == 2) {
		if (strcmp(d, dev_uri) == 0) {
			strcpy(mntpnt, m);
			found = true;
			break;
		}
	}

	fclose(f);

	if (!found) {
		XAL_DEBUG("FAILED: device(%s) not mounted", dev_uri);
		return -EINVAL;
	}

	return 0;
}

/**
 * Create the shared state region for the given shm_name and publish it
 *
 * A secondary attaches using this region alone, so it carries the backend, superblock,
 * mountpoint and the subtree a scoped index is rooted at. On failure nothing is left behind
 * under the name.
 */
static int
publish_shared_state(struct xal *xal, const char *shm_name, const char *mountpoint,
		     enum xal_backend be)
{
	char shm_name_state[XAL_PATH_MAXLEN + 9];
	struct xal_shared_state *state;
	int fd, err;

	snprintf(shm_name_state, sizeof(shm_name_state), "%s_state", shm_name);

	fd = shm_open(shm_name_state, O_CREAT | O_RDWR | O_EXCL, 0644);
	if (fd < 0) {
		XAL_DEBUG("FAILED: shm_open(%s); errno(%d)", shm_name_state, errno);
		return -errno;
	}

	err = ftruncate(fd, sizeof(struct xal_shared_state));
	if (err) {
		XAL_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
		err = -errno;
		close(fd);
		shm_unlink(shm_name_state);
		return err;
	}

	state =
	    mmap(NULL, sizeof(struct xal_shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (state == MAP_FAILED) {
		XAL_DEBUG("FAILED: mmap(); errno(%d)", errno);
		err = -errno;
		shm_unlink(shm_name_state);
		return err;
	}

	xal->state_shm_name = strdup(shm_name_state);
	if (!xal->state_shm_name) {
		XAL_DEBUG("FAILED: strdup(); errno(%d)", errno);
		munmap(state, sizeof(struct xal_shared_state));
		shm_unlink(shm_name_state);
		return -ENOMEM;
	}

	xal->state = state;
	xal->index_state = &state->index_state;
	xal->seq_lock = &state->seq_lock;

	/* ftruncate() zero-fills, and XAL_STATE_CLEAN is zero, so the region would read as
	 * an up-to-date index before one has been built. Mark it dirty here; xal_index()
	 * clears it once there is something to attach to. */
	atomic_store(&state->index_state, XAL_STATE_DIRTY);

	state->version = XAL_SHM_VERSION;
	state->type = be;
	state->sb = xal->sb;
	strncpy(state->mountpoint, mountpoint, XAL_PATH_MAXLEN - 1);
	state->mountpoint[XAL_PATH_MAXLEN - 1] = '\0';

	if (be == XAL_BACKEND_FIEMAP) {
		struct xal_be_fiemap *fiemap_be = (struct xal_be_fiemap *)&xal->be;

		if (fiemap_be->subtree) {
			strncpy(state->subtree, fiemap_be->subtree, XAL_PATH_MAXLEN - 1);
			state->subtree[XAL_PATH_MAXLEN - 1] = '\0';
		}
	}

	atomic_store_explicit(&state->magic, XAL_SHM_MAGIC, memory_order_release);

	return 0;
}

/**
 * A large part of this code is duplicated from xal_open(), but it allows
 * users to open a xal struct with FIEMAP without the dependency on xNVMe.
 * Note that as it does not set the superblock lba sizea, meaning a call
 * to xal_extent_in_lba() would not work.
 */
int
xal_open_from_uri(const char *uri, struct xal **xal, struct xal_opts *opts)
{
	struct xal_opts opts_default = {0};
	char mountpoint[XAL_PATH_MAXLEN + 1] = {0};
	int err;

	if (!uri) {
		return -EINVAL;
	}

	if (!opts) {
		opts = &opts_default;
	}

	/* No device means no on-disk format to read, so FIEMAP is the only reachable backend. */
	if (opts->be && (opts->be != XAL_BACKEND_FIEMAP)) {
		XAL_DEBUG("FAILED: backend(%d) needs a device, use xal_open()", opts->be);
		return -EINVAL;
	}
	opts->be = XAL_BACKEND_FIEMAP;

	if (opts->mountpoint && strlen(opts->mountpoint)) {
		strncpy(mountpoint, opts->mountpoint, XAL_PATH_MAXLEN);
		mountpoint[XAL_PATH_MAXLEN] = '\0';
	} else {
		err = retrieve_mountpoint(uri, mountpoint);
		if (err) {
			XAL_DEBUG("FAILED: retrieve_mountpoint(%s); err(%d)", uri, err);
			return err;
		}
	}

	err = xal_be_fiemap_open(xal, mountpoint, opts);
	if (err) {
		XAL_DEBUG("FAILED: xal_be_fiemap_open(); err(%d)", err);
		return err;
	}

	if (opts->shm_name) {
		err = publish_shared_state(*xal, opts->shm_name, mountpoint, opts->be);
		if (err) {
			XAL_DEBUG("FAILED: publish_shared_state(); err(%d)", err);
			xal_close(*xal);
			*xal = NULL;
			return err;
		}
	}

	return 0;
}

int
xal_open(struct xnvme_dev *dev, struct xal **xal, struct xal_opts *opts)
{
	const struct xnvme_ident *ident;
	const struct xnvme_spec_idfy_ns *ns;
	struct xal_opts opts_default = {0};
	char mountpoint[XAL_PATH_MAXLEN + 1] = {0};
	uint8_t fidx;
	int err;

	if (!dev) {
		return -EINVAL;
	}

	if (!opts) {
		opts = &opts_default;
	}

	ident = xnvme_dev_get_ident(dev);
	if (!ident) {
		XAL_DEBUG("FAILED: xnvme_dev_get_ident()");
		return -EINVAL;
	}

	if (opts->mountpoint && strlen(opts->mountpoint)) {
		strncpy(mountpoint, opts->mountpoint, XAL_PATH_MAXLEN);
		mountpoint[XAL_PATH_MAXLEN] = '\0';
	}

	if (!opts->be) {
		if (!strlen(mountpoint)) {
			err = retrieve_mountpoint(ident->uri, mountpoint);
			if (err) {
				XAL_DEBUG("INFO: Failed retrieve_mountpoint(), this is OK, setting backend to XFS");
				opts->be = XAL_BACKEND_XFS;
				err = 0;
			} else {
				XAL_DEBUG("INFO: dev(%s) mounted at path(%s), setting backend to FIEMAP", ident->uri, mountpoint);
				opts->be = XAL_BACKEND_FIEMAP;
			}
		} else {
			XAL_DEBUG("INFO: given mountpoint at path(%s), setting backend to FIEMAP", mountpoint);
			opts->be = XAL_BACKEND_FIEMAP;
		}
	}

	switch (opts->be) {
		case XAL_BACKEND_XFS:
			err = xal_be_xfs_open(dev, xal, opts);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_xfs_open(); err(%d)", err);
				return err;
			}

			break;

		case XAL_BACKEND_FIEMAP:
			if (strlen(mountpoint) == 0) {
				err = retrieve_mountpoint(ident->uri, mountpoint);
				if (err) {
					XAL_DEBUG("FAILED: retrieve_mountpoint(); err(%d)", err);
					return err;
				}
			}

			err = xal_be_fiemap_open(xal, mountpoint, opts);
			if (err) {
				XAL_DEBUG("FAILED: xal_be_fiemap_open(); err(%d)", err);
				return err;
			}

			break;

		default:
			XAL_DEBUG("FAILED: Unexpected backend(%d)", opts->be);
			return -EINVAL;
	}

	(*xal)->dev = dev;

	ns = xnvme_dev_get_ns(dev);
	if (!ns) {
		err = -errno;
		XAL_DEBUG("FAILED: xnvme_dev_get_ns(); err(%d)", err);
		xal_close(*xal);
		*xal = NULL;
		return err;
	}

	fidx = ns->flbas.format;
	if (ns->nlbaf > 16) {
		fidx += ns->flbas.format_msb << 4;
	}

	(*xal)->sb.lba_blksze = 1U << ns->lbaf[fidx].ds;

	if (opts->shm_name) {
		err = publish_shared_state(*xal, opts->shm_name, mountpoint, opts->be);
		if (err) {
			XAL_DEBUG("FAILED: publish_shared_state(); err(%d)", err);
			xal_close(*xal);
			*xal = NULL;
			return err;
		}
	}

	return 0;
}

int
xal_index(struct xal *xal)
{
	struct xal_backend_base *be = (struct xal_backend_base *)&xal->be;

	if (xal->procrole == XAL_PROCROLE_SECONDARY) {
		return -EINVAL;
	}

	return be->index(xal);
}

static int
_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data, int depth)
{
	int err;

	if (xal_is_dirty(xal)) {
		XAL_DEBUG("FAILED: File system has changed");
		return -ESTALE;
	}

	if (cb_func) {
		err = cb_func(xal, inode, cb_data, depth);
		if (err) {
			return err;
		}
	}

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR: {
		struct xal_inode *inodes = xal_inode_at(xal, inode->content.dentries.inodes_idx);

		for (uint32_t i = 0; i < inode->content.dentries.count; ++i) {
			err = _walk(xal, &inodes[i], cb_func, cb_data, depth + 1);
			if (err) {
				return err;
			}
		}
	} break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		return 0;

	default:
		XAL_DEBUG("FAILED: Unknown / unsupported ftype: %d", inode->ftype);
		return -EINVAL;
	}

	return 0;
}

int
xal_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data)
{
	if (xal_is_dirty(xal)) {
		XAL_DEBUG("FAILED: File system has changed");
		return -ESTALE;
	}

	return _walk(xal, inode, cb_func, cb_data, 0);
}

struct xal_inode *
xal_get_root(struct xal *xal)
{
	if (xal->root_idx == XAL_POOL_IDX_NONE) {
		return NULL;
	}
	return xal_inode_at(xal, xal->root_idx);
}

bool
xal_is_dirty(struct xal *xal)
{
	return atomic_load(xal->index_state) != XAL_STATE_CLEAN;
}

void
xal_mark_dirty(struct xal *xal)
{
	atomic_store(xal->index_state, XAL_STATE_DIRTY);
}

void
xal_mark_index_done(struct xal *xal, int err)
{
	if (err) {
		atomic_store(xal->index_state, XAL_STATE_DIRTY);
		return;
	}

	// A mark landing mid-rebuild wins the exchange and survives the index.
	int expected = XAL_STATE_INDEXING;

	atomic_compare_exchange_strong(xal->index_state, &expected, XAL_STATE_CLEAN);
}

int
xal_get_seq_lock(struct xal *xal)
{
	return atomic_load(xal->seq_lock);
}

const struct xal_sb *
xal_get_sb(struct xal *xal)
{
	return &xal->sb;
}

uint32_t
xal_get_sb_blocksize(struct xal *xal)
{
	return xal->sb.blocksize;
}

int
xal_from_shm(const char *shm_name, struct xal **out)
{
	struct xal *xal;
	struct xal_shared_state *state;
	struct stat st;
	char shm_name_inodes[128], shm_name_extents[128], shm_name_state[128];
	size_t inodes_size, extents_size;
	void *inodes_mem, *extents_mem;
	uint64_t magic;
	int shm_fd = -1, err;

	xal = calloc(1, sizeof(*xal));
	if (!xal) {
		return -ENOMEM;
	}

	xal->procrole = XAL_PROCROLE_SECONDARY;

	snprintf(shm_name_inodes, sizeof(shm_name_inodes), "%s_inodes", shm_name);
	snprintf(shm_name_extents, sizeof(shm_name_extents), "%s_extents", shm_name);
	snprintf(shm_name_state, sizeof(shm_name_state), "%s_state", shm_name);

	/* STATE */
	shm_fd = shm_open(shm_name_state, O_RDWR, 0);
	if (shm_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: shm_open(state); err(%d)\n", err);
		goto failed;
	}

	err = fstat(shm_fd, &st);
	if (err) {
		err = -errno;
		fprintf(stderr, "Failed: fstat(state); err(%d)\n", err);
		goto failed;
	}

	if (!st.st_size) {
		XAL_DEBUG("FAILED: state region is empty, primary is still publishing it");
		err = -EAGAIN;
		goto failed;
	}
	if (st.st_size != (off_t)sizeof(struct xal_shared_state)) {
		fprintf(stderr, "Failed: state region is %jd bytes, expected %zu\n",
			(intmax_t)st.st_size, sizeof(struct xal_shared_state));
		err = -EPROTO;
		goto failed;
	}

	state = mmap(NULL, sizeof(struct xal_shared_state), PROT_READ | PROT_WRITE, MAP_SHARED,
		     shm_fd, 0);
	close(shm_fd);
	shm_fd = -1;

	if (state == MAP_FAILED) {
		err = -errno;
		fprintf(stderr, "Failed: mmap(state); err(%d)\n", err);
		goto failed;
	}

	magic = atomic_load_explicit(&state->magic, memory_order_acquire);
	if (!magic) {
		XAL_DEBUG("FAILED: state region has no magic, primary is still publishing it");
		err = -EAGAIN;
		goto unmap_state;
	}
	if ((magic != XAL_SHM_MAGIC) || (state->version != XAL_SHM_VERSION)) {
		fprintf(stderr, "Failed: state magic(%llx) version(%u), expected magic(%llx) "
				"version(%u)\n",
			(unsigned long long)magic, state->version,
			(unsigned long long)XAL_SHM_MAGIC, XAL_SHM_VERSION);
		err = -EPROTO;
		goto unmap_state;
	}

	xal->state = state;
	xal->index_state = &state->index_state;
	xal->seq_lock = &state->seq_lock;
	xal->sb = state->sb;

	if (xal_is_dirty(xal)) {
		err = -ESTALE;
		goto unmap_state;
	}

	/* INODES */
	shm_fd = shm_open(shm_name_inodes, O_RDONLY, 0);
	if (shm_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: shm_open(inodes); err(%d)\n", err);
		goto unmap_state;
	}

	err = fstat(shm_fd, &st);
	if (err) {
		err = -errno;
		fprintf(stderr, "Failed: fstat(inodes); err(%d)\n", err);
		goto unmap_state;
	}

	inodes_size = st.st_size;
	inodes_mem = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	shm_fd = -1;

	if (inodes_mem == MAP_FAILED) {
		err = -errno;
		fprintf(stderr, "Failed: mmap(inodes); err(%d)\n", err);
		goto unmap_state;
	}

	xal->inodes.memory = inodes_mem;
	xal->inodes.element_size = sizeof(struct xal_inode);
	xal->inodes.reserved = inodes_size / xal->inodes.element_size;

	/* EXTENTS */
	shm_fd = shm_open(shm_name_extents, O_RDONLY, 0);
	if (shm_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: shm_open(extents); err(%d)\n", err);
		goto unmap_inodes;
	}

	err = fstat(shm_fd, &st);
	if (err) {
		err = -errno;
		fprintf(stderr, "Failed: fstat(extents); err(%d)\n", err);
		goto unmap_inodes;
	}

	extents_size = st.st_size;
	extents_mem = mmap(NULL, extents_size, PROT_READ, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	shm_fd = -1;

	if (extents_mem == MAP_FAILED) {
		err = -errno;
		fprintf(stderr, "Failed: mmap(extents); err(%d)\n", err);
		goto unmap_inodes;
	}

	xal->extents.memory = extents_mem;
	xal->extents.element_size = sizeof(struct xal_extent);
	xal->extents.reserved = extents_size / xal->extents.element_size;

	if (state->type == XAL_BACKEND_FIEMAP) {
		struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
		be->base.type = XAL_BACKEND_FIEMAP;
		be->base.close = xal_be_fiemap_close;
		be->mountpoint = strdup(state->mountpoint);
		if (!be->mountpoint) {
			err = -ENOMEM;
			goto unmap_extents;
		}

		if (strlen(state->subtree)) {
			be->subtree = strdup(state->subtree);
			if (!be->subtree) {
				free(be->mountpoint);
				err = -ENOMEM;
				goto unmap_extents;
			}
		}
	} else {
		struct xal_be_xfs *be = (struct xal_be_xfs *)&xal->be;
		be->base.type = XAL_BACKEND_XFS;
		be->base.close = xal_be_xfs_close;
	}

	*out = xal;

	return 0;

unmap_extents:
	munmap(extents_mem, extents_size);
unmap_inodes:
	munmap(inodes_mem, inodes_size);
unmap_state:
	munmap(state, sizeof(struct xal_shared_state));
failed:
	free(xal);

	if (shm_fd >= 0) {
		close(shm_fd);
	}

	return err;
}

int
xal_inode_path_pp(struct xal *xal, struct xal_inode *inode)
{
	int wrtn = 0;

	if (!inode) {
		return wrtn;
	}
	if (inode->parent_idx == XAL_POOL_IDX_NONE) {
		return wrtn;
	}

	wrtn += xal_inode_path_pp(xal, xal_inode_at(xal, inode->parent_idx));
	wrtn += printf("/%.*s", inode->namelen, inode->name);

	return wrtn;
}

bool
xal_inode_is_dir(struct xal_inode *inode)
{
	return inode->ftype == XAL_ODF_DIR3_FT_DIR;
}

bool
xal_inode_is_file(struct xal_inode *inode)
{
	return inode->ftype == XAL_ODF_DIR3_FT_REG_FILE;
}

int
xal_extent_in_bytes(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output)
{
	if (!extent) {
		XAL_DEBUG("FAILED: no extent given");
		return -EINVAL;
	}

	output->start_offset = extent->start_offset * xal->sb.blocksize;
	output->size = extent->nblocks * xal->sb.blocksize;
	output->start_block = xal_fsbno_offset(xal, extent->start_block);
	output->unit = XAL_EXTENT_UNIT_BYTES;

	return 0;
}

int
xal_extent_in_lba(struct xal *xal, const struct xal_extent *extent, struct xal_extent_converted *output)
{
	uint32_t lba_blksze = xal->sb.lba_blksze;

	if (!extent) {
		XAL_DEBUG("FAILED: no extent given");
		return -EINVAL;
	}

	if (!lba_blksze) {
		XAL_DEBUG("SKIPPED: cannot convert to lba without lba blksze");
		return -EINVAL;
	}

	output->start_offset = extent->start_offset * xal->sb.blocksize / lba_blksze;
	output->size = extent->nblocks * xal->sb.blocksize / lba_blksze;
	output->start_block = xal_fsbno_offset(xal, extent->start_block) / lba_blksze;
	output->unit = XAL_EXTENT_UNIT_LBA;

	return 0;
}

static int
compare_name_to_inode(const void *key, const void *elem)
{
	const char *component = key;
	const struct xal_inode *inode = elem;

	const char *basename = strrchr(inode->name, '/');
	if (basename) {
		basename++;
	} else {
		basename = inode->name;
	}

	return strcmp(component, basename);
}

int
search_by_traversal(struct xal *xal, struct xal_inode *root, char *path, char *basepath, struct xal_inode **inode)
{
	struct xal_inode *search, *found = NULL;
	char *search_begin, *search_end;
	size_t basepath_len;

	basepath_len = strlen(basepath);

	if (!root) {
		XAL_DEBUG("FAILED: no xal->root, call xal_index()");
		return -EINVAL;
	}

	if (strlen(path) <= basepath_len + 1) {
		XAL_DEBUG("FAILED: Not a valid path(%s); path too short; must be absolute path to entry in mountpoint(%s)",
			path, basepath);
		return -EINVAL;
	}

	if (strncmp(path, basepath, basepath_len) != 0) {
		XAL_DEBUG("FAILED: Not a valid path(%s); not a subpath; must be absolute path to entry in mountpoint(%s)",
			path, basepath);
		return -EINVAL;
	}

	search = root;
	search_begin = path + basepath_len + 1;
	search_end = strchr(search_begin, '/');

	while (!found) {
		struct xal_inode *child;
		size_t search_len = search_end ? (size_t)(search_end - search_begin) : strlen(search_begin);
		char component[search_len + 1];

		memcpy(component, search_begin, search_len);
		component[search_len] = '\0';

		XAL_DEBUG("Searching for component(%s)", component);

		child = bsearch(component, xal_inode_at(xal, search->content.dentries.inodes_idx),
				search->content.dentries.count, sizeof(struct xal_inode), compare_name_to_inode);

		if (!child) {
			XAL_DEBUG("Component(%s) not found", component);
			break;
		}

		if (!search_end) {
			XAL_DEBUG("Final component(%s) found", component);
			found = child;
		} else {
			XAL_DEBUG("Component(%s) found, continuing", component);
			search = child;
			search_begin = search_end + 1;
			search_end = strchr(search_begin, '/');
		}
	}

	if (!found) {
		XAL_DEBUG("FAILED: Inode not found");
		return -ENOENT;
	}

	*inode = found;

	return 0;
}

int
xal_get_inode(struct xal *xal, char *path, struct xal_inode **inode)
{
	struct xal_backend_base *be;
	int err = 0;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	if (!path) {
		XAL_DEBUG("FAILED: no path given");
		return -EINVAL;
	}

	if (xal_is_dirty(xal)) {
		XAL_DEBUG("FAILED: File system has changed");
		return -ESTALE;
	}

	if (xal->root_idx == XAL_POOL_IDX_NONE) {
		XAL_DEBUG("FAILED: Missing call to xal_index()");
		return -EINVAL;
	}

	be = (struct xal_backend_base *)&xal->be;

	switch (be->type) {
	case XAL_BACKEND_XFS:
		return search_by_traversal(xal, xal_inode_at(xal, xal->root_idx), path, "", inode);
	case XAL_BACKEND_FIEMAP:
		return xal_be_fiemap_get_inode(xal, path, inode);
	default:
		XAL_DEBUG("Failed: Unknown backend type(%d)", be->type);
		err = -EINVAL;
	}

	return err;
}

int
xal_get_extents(struct xal *xal, char *path, struct xal_extents **extents)
{
	struct xal_inode *inode;
	int err;

	err = xal_get_inode(xal, path, &inode);
	if (err) {
		XAL_DEBUG("FAILED: xal_get_inode(); err(%d)", err);
		return err;
	}

	if (!xal_inode_is_file(inode)) {
		XAL_DEBUG("FAILED: inode at given path is not a file");
		return -EINVAL;
	}

	*extents = &inode->content.extents;

	return 0;
}

int
xal_get_dentries(struct xal *xal, char *path, struct xal_dentries **dentries)
{
	struct xal_inode *inode;
	int err;

	err = xal_get_inode(xal, path, &inode);
	if (err) {
		XAL_DEBUG("FAILED: xal_get_inode(); err(%d)", err);
		return err;
	}

	if (!xal_inode_is_dir(inode)) {
		XAL_DEBUG("FAILED: inode at given path is not a directory");
		return -ENOTDIR;
	}

	*dentries = &inode->content.dentries;

	return 0;
}
