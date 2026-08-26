#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xal_pool.h>

int
xal_pool_unmap(struct xal_pool *pool, bool unlink)
{
	int ret = 0;
	int err;

	if (pool->memory) {
		err = munmap(pool->memory, pool->reserved * pool->element_size);
		if (err < 0) {
			ret = -errno;
			XAL_DEBUG("FAILED: munmap(...); errno(%d)", errno);
		} else {
			pool->memory = NULL;
		}
	}

	if (pool->shm_name) {
		if (unlink) {
			err = shm_unlink(pool->shm_name);
			if (err < 0) {
				XAL_DEBUG("FAILED: shm_unlink(...); errno(%d)", errno);
				if (!ret) {
					ret = -errno;
				}
			}
		}

		free(pool->shm_name);
		pool->shm_name = NULL;
	}

	return ret;
}

int
xal_pool_grow(struct xal_pool *pool, size_t growby)
{
	size_t growby_nbytes = growby * pool->element_size;
	size_t allocated_nbytes = growby_nbytes + pool->allocated * pool->element_size;

	if (mprotect(pool->memory, allocated_nbytes, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}

	pool->allocated += growby;

	return 0;
}

int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size,
             const char *shm_name)
{
	size_t nbytes = reserved * element_size;
	int err;

	if (pool->reserved) {
		XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
		return -EINVAL;
	}

	pool->reserved = reserved;
	pool->element_size = element_size;
	pool->free = 0;

	if (shm_name) {
		int fd;

		pool->shm_name = strdup(shm_name);
		if (!pool->shm_name) {
			XAL_DEBUG("FAILED: strdup(); errno(%d)", errno);
			err = -errno;
			goto failed;
		}

		fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0644);
		if (fd < 0) {
			XAL_DEBUG("FAILED: shm_open(%s); errno(%d)", shm_name, errno);
			err = -errno;
			goto failed_name;
		}

		err = ftruncate(fd, nbytes);
		if (err) {
			XAL_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
			err = -errno;
			close(fd);
			goto failed_created;
		}

		pool->memory = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (pool->memory == MAP_FAILED) {
			XAL_DEBUG("FAILED: mmap(); errno(%d)", errno);
			err = -errno;
			pool->memory = NULL;
			goto failed_created;
		}

		pool->allocated = reserved;
		pool->growby = reserved;
	} else {
		if (allocated > reserved) {
			XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
			err = -EINVAL;
			goto failed;
		}

		pool->allocated = 0;
		pool->growby = allocated;

		pool->memory =
		    mmap(NULL, nbytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (MAP_FAILED == pool->memory) {
			XAL_DEBUG("FAILED: mmap(...); errno(%d)", errno);
			err = -errno;
			pool->memory = NULL;
			goto failed;
		}

		err = xal_pool_grow(pool, allocated);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(...); err(%d)", err);
			xal_pool_unmap(pool, false);
			goto failed;
		}
	}

	return 0;

failed_created:
	shm_unlink(pool->shm_name);
failed_name:
	free(pool->shm_name);
	pool->shm_name = NULL;
failed:
	pool->reserved = 0;
	pool->allocated = 0;
	pool->growby = 0;
	pool->free = 0;
	pool->element_size = 0;

	return err;
}

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, uint32_t *idx)
{
	int err;

	/* A single claim may exceed growby; grow by as much as this claim needs. The only
	 * hard limit is the reserved virtual range. */
	if (pool->free + count > pool->reserved) {
		XAL_DEBUG("FAILED: claim(%zu) exceeds reserved(%zu)", count, pool->reserved);
		return -ENOMEM;
	}

	/* Reject before growing: indices are uint32_t. Only bites when a caller sets
	 * reserved > UINT32_MAX; otherwise the reserved check above already caught it. */
	if (pool->free + count > UINT32_MAX) {
		XAL_DEBUG("FAILED: pool->free exceeds uint32_t range");
		return -EOVERFLOW;
	}

	if ((pool->free + count) > pool->allocated) {
		size_t need = (pool->free + count) - pool->allocated;
		size_t by = need > pool->growby ? need : pool->growby;

		if (pool->allocated + by > pool->reserved) {
			by = pool->reserved - pool->allocated;
		}

		err = xal_pool_grow(pool, by);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (idx) {
		*idx = pool->free;
	}
	pool->free += count;

	return 0;
}

int
xal_pool_claim_extents(struct xal_pool *pool, size_t count, uint32_t *idx)
{
	int err;

	/* A single claim may exceed growby (e.g. one heavily-fragmented file with more
	 * extents than the tree has entries). Grow by as much as this claim needs; the only
	 * hard limit is the reserved virtual range. */
	if (pool->free + count > pool->reserved) {
		XAL_DEBUG("FAILED: claim(%zu) exceeds reserved(%zu)", count, pool->reserved);
		return -ENOMEM;
	}

	/* Reject before growing: indices are uint32_t. Only bites when a caller sets
	 * reserved > UINT32_MAX; otherwise the reserved check above already caught it. */
	if (pool->free + count > UINT32_MAX) {
		XAL_DEBUG("FAILED: pool->free exceeds uint32_t range");
		return -EOVERFLOW;
	}

	if ((pool->free + count) > pool->allocated) {
		size_t need = (pool->free + count) - pool->allocated;
		size_t by = need > pool->growby ? need : pool->growby;

		if (pool->allocated + by > pool->reserved) {
			by = pool->reserved - pool->allocated;
		}

		err = xal_pool_grow(pool, by);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (idx) {
		*idx = pool->free;
	}
	pool->free += count;

	return 0;
}

int
xal_pool_clear(struct xal_pool *pool)
{
	/* Indices are only handed out sequentially from free, so nothing past it was written.
	 * allocated is left as it is, keeping the PROT_NONE guard beyond it. */
	memset(pool->memory, 0, pool->free * pool->element_size);

	pool->free = 0;

	return 0;
}
