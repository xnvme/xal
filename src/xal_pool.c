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
	int err;
	
	err = munmap(pool->memory, pool->reserved * pool->element_size);
	if (err < 0) {
		err = -errno;
		XAL_DEBUG("FAILED: mprotmunmapect(...); errno(%d)", errno);
		return err;
	}

	if (pool->shm_name && unlink) {
		err = shm_unlink(pool->shm_name);
		if (err < 0) {
			err = -errno;
			XAL_DEBUG("FAILED: shm_unlink(...); errno(%d)", errno);
			return err;
		}
	}

	return 0;
}

int
xal_pool_grow(struct xal_pool *pool, size_t growby)
{
	size_t growby_nbytes = growby * pool->element_size;
	size_t allocated_nbytes = growby_nbytes + pool->allocated * pool->element_size;
	uint8_t *cursor = pool->memory;

	if (mprotect(pool->memory, allocated_nbytes, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}
	memset(&cursor[pool->free * pool->element_size], 0, growby_nbytes);

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

		fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
		if (fd < 0) {
			XAL_DEBUG("FAILED: shm_open(%s); errno(%d)", shm_name, errno);
			return -errno;
		}

		err = ftruncate(fd, nbytes);
		if (err) {
			XAL_DEBUG("FAILED: ftruncate(); errno(%d)", errno);
			close(fd);
			return -errno;
		}

		pool->memory = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (pool->memory == MAP_FAILED) {
			XAL_DEBUG("FAILED: mmap(); errno(%d)", errno);
			return -errno;
		}
		memset(pool->memory, 0, nbytes);

		pool->allocated = reserved;
		pool->growby = reserved;
		
		pool->shm_name = strdup(shm_name);
		if (!pool->shm_name) {
			XAL_DEBUG("FAILED: strdup(); errbo(%d)", errno);
			return -errno;
		}
	} else {
		if (allocated > reserved) {
			XAL_DEBUG("FAILED: xal_pool_map(...); errno(%d)", EINVAL);
			return -EINVAL;
		}

		pool->allocated = 0;
		pool->growby = allocated;

		pool->memory =
		    mmap(NULL, nbytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (MAP_FAILED == pool->memory) {
			XAL_DEBUG("FAILED: mmap(...); errno(%d)", errno);
			return -errno;
		}

		err = xal_pool_grow(pool, allocated);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(...); err(%d)", err);
			xal_pool_unmap(pool, false);
			return err;
		}
	}

	return 0;
}

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, uint32_t *idx)
{
	int err;

	if (count > pool->growby) {
		XAL_DEBUG("FAILED: count > pool->growby");
		return -EINVAL;
	}

	if (pool->allocated <= (pool->free + count)) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("FAILED: xal_pool_grow(); err(%d)", err);
			return err;
		}
	}

	if (pool->free + count > UINT32_MAX) {
		XAL_DEBUG("FAILED: pool->free exceeds uint32_t range");
		return -EOVERFLOW;
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

	if (count > pool->growby) {
		XAL_DEBUG("FAILED: count > pool->growby");
		return -EINVAL;
	}

	if (pool->allocated <= (pool->free + count)) {
		err = xal_pool_grow(pool, pool->growby);
		if (err) {
			XAL_DEBUG("xal_pool_grow(); err(%d)", err);
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
	if (mprotect(pool->memory, pool->reserved * pool->element_size, PROT_READ | PROT_WRITE)) {
		XAL_DEBUG("FAILED: mprotect(...); errno(%d)", errno);
		return -errno;
	}
	memset(pool->memory, 0, pool->reserved * pool->element_size);

	pool->free = 0;
	pool->allocated = 0;

	return 0;
}
