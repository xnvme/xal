#ifndef XAL_BE_FIEMAP_LEASE_H
#define XAL_BE_FIEMAP_LEASE_H

#include <khash.h>
#include <libxal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * Per-file state in XAL_WATCHMODE_READ_LEASE
 *
 * UNMAPPED -> MAPPING -> VALID on the first xal_extents_acquire(). VALID -> REVOKING when the
 * kernel breaks the lease or a release finds the extents stale, and REVOKING -> REVOKED once the
 * last reader has released and the lease is dropped. VALID -> EVICTING -> UNMAPPED when xal drops
 * the lease of a file without readers to stay within lease_max, or back to VALID if a reader came
 * meanwhile. UNSEALED marks a file that was open for writing when a lease was attempted. REVOKED
 * and UNSEALED files are mapped again by the next xal_extents_acquire() that can take a lease.
 */
enum xal_lease_state {
	XAL_LEASE_UNMAPPED = 0,
	XAL_LEASE_MAPPING = 1,
	XAL_LEASE_VALID = 2,
	XAL_LEASE_REVOKING = 3,
	XAL_LEASE_REVOKED = 4,
	XAL_LEASE_UNSEALED = 5,
	XAL_LEASE_EVICTING = 6,
};

struct xal_lease_entry {
	int fd;		     ///< Holds the lease and keeps the inode alive; -1 when not held
	atomic_int state;    ///< One of enum xal_lease_state
	atomic_uint readers; ///< Callers between xal_extents_acquire() and xal_extents_release()
	uint32_t extent_cap; ///< Extents the region at inode->content.extents.extent_idx can hold
	_Atomic uint64_t expire_ns; ///< CLOCK_MONOTONIC time from which a broken lease may have
				    ///< timed out; 0 when it cannot, or after a stale release
	atomic_bool stale;	    ///< A release found the extents may no longer hold the data
	atomic_bool ref;	    ///< Acquired since the eviction hand last passed
	uint32_t slot;		    ///< Position in held[] while fd is open
};

KHASH_MAP_INIT_INT(fd_to_idx, uint32_t)

struct xal_lease_set {
	struct xal *xal;
	unsigned generation; ///< Of the process that took the leases; see lease_foreign()
	dev_t dev;	     ///< Device of the mountpoint; a file elsewhere is not mapped
	struct xal_lease_entry *entries; ///< Indexed like the inode pool
	size_t nentries;
	kh_fd_to_idx_t *fd_map; ///< Lease fd to inode index, for the break notification
	uint32_t *held;		///< Entries holding a lease fd, in no order
	uint32_t nheld;
	uint32_t held_cap;
	uint32_t max_held; ///< Past it, the lease of a file without readers is dropped
	uint32_t hand;	   ///< Next position in held[] the eviction looks at
	int *dead;	   ///< Lease fds dropped under lock, closed by lease_unlock()
	uint32_t ndead;
	uint32_t dead_cap;
	pthread_mutex_t lock; ///< Guards fd_map, held[], the extents pool and publishing a mapping
	struct xal_lease_stats stats;
	struct xal_lease_set *next; ///< In the process-wide list of sets
	bool recheck;		    ///< !xal_opts.lease_no_recheck
};

int
xal_be_fiemap_lease_init(struct xal *xal, const char *mountpoint, const struct xal_opts *opts);

/**
 * Release every lease and forget every mapping; for xal_index() before it clears the pools
 *
 * Returns -EBUSY, and leaves everything as is, when a caller holds extents and force is false.
 */
int
xal_be_fiemap_lease_reset(struct xal *xal, bool force);

/**
 * Size the per-file state to the inode pool; for xal_index() after the walk
 */
int
xal_be_fiemap_lease_prepare(struct xal *xal);

void
xal_be_fiemap_lease_close(struct xal *xal);

#endif /* XAL_BE_FIEMAP_LEASE_H */
