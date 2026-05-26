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

	if (!xal) {
		return;
	}

	xal_pool_unmap(&xal->inodes);
	xal_pool_unmap(&xal->extents);

	if (xal->dirty != &xal->_dirty_storage) {
		munmap(xal->dirty, sizeof(atomic_bool));
	}

	be = (struct xal_backend_base *)&xal->be;
	if (be->close) {
		be->close(xal);
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

	if (!opts->be) {
		err = retrieve_mountpoint(ident->uri, mountpoint);
		if (err) {
			XAL_DEBUG("INFO: Failed retrieve_mountpoint(), this is OK");
			opts->be = XAL_BACKEND_XFS;
			err = 0;
		} else {
			XAL_DEBUG("INFO: dev(%s) mounted at path(%s)", ident->uri, mountpoint);
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

	if (opts->shm_name) {
		char shm_name[XAL_PATH_MAXLEN + 9];
		int fd;
		void *mem;

		snprintf(shm_name, sizeof(shm_name), "%s_dirty", opts->shm_name);

		fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
		if (fd < 0) {
			XAL_DEBUG("FAILED: shm_open(%s); errno(%d)", shm_name, errno);
			xal_close(*xal);
			return -errno;
		}

		err = ftruncate(fd, sizeof(atomic_bool));
		if (err) {
			XAL_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
			close(fd);
			xal_close(*xal);
			return -errno;
		}

		mem = mmap(NULL, sizeof(atomic_bool), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (mem == MAP_FAILED) {
			XAL_DEBUG("FAILED: mmap(); errno(%d)", errno);
			xal_close(*xal);
			return -errno;
		}

		(*xal)->dirty = mem;
	}

	ns = xnvme_dev_get_ns(dev);
	if (!ns) {
		err = -errno;
		XAL_DEBUG("FAILED: xnvme_dev_get_ns(); err(%d)", err);
		return err;
	}

	fidx = ns->flbas.format;
	if (ns->nlbaf > 16) {
		fidx += ns->flbas.format_msb << 4;
	}

	(*xal)->sb.lba_blksze = 1U << ns->lbaf[fidx].ds;

	return 0;
}

int
xal_index(struct xal *xal)
{
	struct xal_backend_base *be = (struct xal_backend_base *)&xal->be;

	if (xal->shared_view) {
		return -EINVAL;
	}

	return be->index(xal);
}

static int
_walk(struct xal *xal, struct xal_inode *inode, xal_walk_cb cb_func, void *cb_data, int depth)
{
	int err;

	if (atomic_load(xal->dirty)) {
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
	return _walk(xal, inode, cb_func, cb_data, 0);
}

struct xal_inode *
xal_get_root(struct xal *xal)
{
	return xal_inode_at(xal, xal->root_idx);
}

bool
xal_is_dirty(struct xal *xal)
{
	return atomic_load(xal->dirty);
}

int
xal_get_seq_lock(struct xal *xal)
{
	return atomic_load(&xal->seq_lock);
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
xal_from_shm(const char *shm_name, const struct xal_sb *sb, const char *mountpoint, struct xal **out)
{
	struct xal *xal;
	struct stat st;
	char shm_name_inodes[128], shm_name_extents[128], shm_name_dirty[128];
	size_t inodes_size, extents_size;
	_Atomic bool *dirty;
	void *inodes_mem, *extents_mem;
	int shm_fd = -1, err;

	xal = calloc(1, sizeof(*xal));
	if (!xal) {
		return -ENOMEM;
	}

	xal->sb = *sb;
	xal->root_idx = 0;
	xal->shared_view = true;

	snprintf(shm_name_inodes, sizeof(shm_name_inodes), "%s_inodes", shm_name);
	snprintf(shm_name_extents, sizeof(shm_name_extents), "%s_extents", shm_name);
	snprintf(shm_name_dirty, sizeof(shm_name_dirty), "%s_dirty", shm_name);

	/* DIRTY */
	shm_fd = shm_open(shm_name_dirty, O_RDONLY, 0);
	if (shm_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: shm_open(dirty); err(%d)\n", err);
		goto failed;
	}

	dirty = mmap(NULL, sizeof(atomic_bool), PROT_READ, MAP_SHARED, shm_fd, 0);

	close(shm_fd);
	shm_fd = -1;

	if (dirty == MAP_FAILED) {
		err = -errno;
		fprintf(stderr, "Failed: mmap(dirty); err(%d)\n", err);
		goto failed;
	}

	xal->dirty = dirty;

	if (atomic_load(xal->dirty)) {
		err = -EINVAL;
		goto unmap_dirty;
	}

	/* INODES */
	shm_fd = shm_open(shm_name_inodes, O_RDONLY, 0);
	if (shm_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: shm_open(inodes); err(%d)\n", err);
		goto unmap_dirty;
	}

	err = fstat(shm_fd, &st);
	if (err) {
		err = -errno;
		fprintf(stderr, "Failed: fstat(inodes); err(%d)\n", err);
		goto unmap_dirty;
	}

	inodes_size = st.st_size;
	inodes_mem = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, shm_fd, 0);

	close(shm_fd);
	shm_fd = -1;

	if (inodes_mem == MAP_FAILED) {
		err = -errno;
		fprintf(stderr, "Failed: mmap(inodes); err(%d)\n", err);
		goto unmap_dirty;
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

	if (mountpoint) {
		struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
		be->base.type = XAL_BACKEND_FIEMAP;
		be->base.close = xal_be_fiemap_close;
		be->mountpoint = strdup(mountpoint);
		if (!be->mountpoint) {
			err = -ENOMEM;
			goto unmap_extents;
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
unmap_dirty:
	munmap(dirty, sizeof(atomic_bool));
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

	output->start_offset = extent->start_offset * xal->sb.blocksize / lba_blksze;
	output->size = extent->nblocks * xal->sb.blocksize / lba_blksze;
	output->start_block = xal_fsbno_offset(xal, extent->start_block) / lba_blksze;
	output->unit = XAL_EXTENT_UNIT_LBA;

	return 0;
}
