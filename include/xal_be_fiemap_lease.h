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
 * meanwhile. REFUSED marks a file whose lease could not be taken: open for writing elsewhere, or,
 * for a write lease, open elsewhere at all. REVOKED and REFUSED files are mapped again by the next
 * xal_extents_acquire() that can take a lease. xal_extents_attach_fd() goes from UNMAPPED, VALID,
 * REVOKED or REFUSED through MAPPING to VALID, or to REFUSED or UNMAPPED when it fails;
 * xal_extents_detach_fd() goes through EVICTING to UNMAPPED.
 */
enum xal_lease_state {
	XAL_LEASE_UNMAPPED = 0,
	XAL_LEASE_MAPPING = 1,
	XAL_LEASE_VALID = 2,
	XAL_LEASE_REVOKING = 3,
	XAL_LEASE_REVOKED = 4,
	XAL_LEASE_REFUSED = 5,
	XAL_LEASE_EVICTING = 6,
};

struct xal_lease_entry {
	int fd;		     ///< Holds the lease, and keeps the inode alive if xal's; -1 if none
	atomic_int state;    ///< One of enum xal_lease_state
	atomic_uint readers; ///< Callers between xal_extents_acquire() and xal_extents_release()
	uint32_t extent_cap; ///< Extents the region at inode->content.extents.extent_idx can hold
	_Atomic uint64_t expire_ns; ///< CLOCK_MONOTONIC time from which a broken lease may have
				    ///< timed out; 0 when it cannot, or after a stale release
	atomic_bool stale;	    ///< A release found the extents may no longer hold the data
	atomic_bool ref;	    ///< Acquired since the eviction hand last passed
	uint32_t slot;		    ///< Position in held[], or attached[] for ufd, while held
	int ufd; ///< The caller's descriptor from xal_extents_attach_fd(), never closed; -1 if none
	int ltype;	       ///< F_RDLCK, or F_WRLCK on a caller's descriptor open for writing
	struct timespec ctime; ///< The file's ctime once the lease was taken, for a write lease
};

KHASH_MAP_INIT_INT(fd_to_idx, uint32_t)

struct xal_lease_set {
	struct xal *xal;
	unsigned generation; ///< Of the process that took the leases; see lease_foreign()
	dev_t dev;	     ///< Device of the mountpoint; a file elsewhere is not mapped
	struct xal_lease_entry *entries; ///< Indexed like the inode pool
	size_t nentries;
	kh_fd_to_idx_t *fd_map;	 ///< Lease fd to inode index, for the break notification
	kh_fd_to_idx_t *ufd_map; ///< Attached descriptor to inode index, whatever the state
	uint32_t *held;		 ///< Entries holding a lease on a descriptor of xal's, in no order
	uint32_t nheld;
	uint32_t held_cap;
	uint32_t *attached; ///< Entries holding it on the caller's, never counted or dropped
	uint32_t nattached;
	uint32_t attached_cap;
	uint32_t max_held; ///< Past it, the lease of a file without readers is dropped
	uint32_t hand;	   ///< Next position in held[] the eviction looks at
	int *dead;	   ///< Lease fds dropped under lock, closed by lease_unlock()
	uint32_t ndead;
	uint32_t dead_cap;
	pthread_mutex_t lock; ///< Guards the maps, both lists, ufd, the extents pool and publishing
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
