#include <stdint.h>

/**
 * A pool of mmap backed memory for fixed-size elements.
 *
 * This is utilized for inodes and extents. The useful feature is that we can have a contigous
 * virtual address space which can grow without having to move elements nor change pointers to
 * them, as one would otherwise have to do with malloc()/realloc().
 */
struct xal_pool {
	size_t reserved;     ///< Maximum number of elements in the pool
	size_t allocated;    ///< Number of reserved elements that are allocated
	size_t growby;	     ///< Number of reserved elements to allocate at a time
	size_t free;	     ///< Index / position of the next free element
	size_t element_size; ///< Size of a single element in bytes
	char *shm_name;      ///< Name of shared memory region, may be NULL
	void *memory;	     ///< Memory space for elements
};

int
xal_pool_unmap(struct xal_pool *pool, bool unlink);

/**
 * Initialize the given pool of 'struct xal_inode'
 *
 * This will produce a pool of 'reserved' number of inodes, that is, overcommitted memory which is
 * not usable. A subset of this memory, specifically memory for an 'allocated' amount of inodes is
 * made available for read / write, and written to by "zeroing" out the memory.
 *
 * See the xal_pool_claim() helper, which provides arrays of allocated memory usable for
 * inode-storage. The number of allocated inodes are grown, when claimed, until the reserved space
 * is exhausted.
 *
 * If shm_name is NULL, uses private anonymous memory with lazy mprotect growth.
 * If shm_name is non-NULL, backs the pool with a POSIX shared memory object of that name.
 * In the shm case the full reserved size is committed upfront.
 */
int
xal_pool_map(struct xal_pool *pool, size_t reserved, size_t allocated, size_t element_size,
             const char *shm_name);

/**
 *
 */
int
xal_pool_claim_extents(struct xal_pool *pool, size_t count, uint32_t *idx);

int
xal_pool_claim_inodes(struct xal_pool *pool, size_t count, uint32_t *idx);

int
xal_pool_clear(struct xal_pool *pool);
