#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_lease.h>
#include <xal_pool.h>

#define XAL_FIEMAP_BATCH 256

/** Offset from SIGRTMIN of the signal set with F_SETSIG on every lease fd */
#define XAL_LEASE_SIGOFF 7

/** Taken off lease-break-time for the time a break takes to reach xal's thread */
#define XAL_LEASE_EXPIRE_MARGIN_NS (250ULL * 1000 * 1000)

static const struct timespec lease_nap = {.tv_sec = 0, .tv_nsec = 20 * 1000};

/** Longest wait between looks at a file another thread maps, which writes its dirty data back */
#define XAL_LEASE_NAP_MAX_NS (1000 * 1000)

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* The last /proc/sys/fs/lease-break-time read, for when it cannot be, as with no fd free */
static atomic_long lease_break_secs = 45;

static long
lease_break_time(void)
{
	FILE *f = fopen("/proc/sys/fs/lease-break-time", "re");
	long secs;

	if (f) {
		if (fscanf(f, "%ld", &secs) == 1) {
			atomic_store(&lease_break_secs, secs);
		}
		fclose(f);
	}

	return atomic_load(&lease_break_secs);
}

/*
 * When a lease broken now may have timed out: past /proc/sys/fs/lease-break-time seconds the
 * kernel lets the breaker through even with readers left. 0 when it never does, as with a
 * lease-break-time of 0 or less.
 */
static uint64_t
lease_expire_ns(void)
{
	uint64_t now = now_ns(), wait;
	long secs = lease_break_time();

	if (secs <= 0) {
		return 0;
	}

	wait = (uint64_t)secs * 1000000000ULL;

	return now + (wait > XAL_LEASE_EXPIRE_MARGIN_NS ? wait - XAL_LEASE_EXPIRE_MARGIN_NS : 0);
}

/*
 * A broken lease is reported with a signal to the file's owner, which xal sets to one thread of
 * its own with F_SETOWN_EX before taking the lease. The thread blocks every signal and takes these
 * with sigtimedwait(), so no signal disposition of the process changes, and the notification does
 * not depend on the caller's threads, which may exit.
 * The one thread serves every set in the process.
 */
static struct {
	pthread_mutex_t lifecycle; ///< Serializes registering and unregistering sets
	pthread_mutex_t lock;	   ///< Guards the list of sets; taken before any ls->lock
	struct xal_lease_set *sets;
	unsigned nsets;
	pthread_t thread;
	atomic_int tid; ///< Of the thread, set once it runs
	atomic_bool stop;
	int sig;
} lease_proc = {
    .lifecycle = PTHREAD_MUTEX_INITIALIZER,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static pthread_once_t lease_atfork_once = PTHREAD_ONCE_INIT;
/* What pthread_atfork() returned; without the handlers a forked child is not told apart */
static int lease_atfork_err;

/* Bumped in a forked child, which tells the sets it inherits apart without a getpid() call */
static unsigned lease_generation;

/*
 * A forked child inherits its parent's open files, and with them the leases, but not its thread.
 * lease_atfork_child() closes those of the sets it inherits, which then refuse to work, see
 * lease_foreign(); the child starts with no sets.
 */
static void
lease_atfork_prepare(void)
{
	pthread_mutex_lock(&lease_proc.lifecycle);
	pthread_mutex_lock(&lease_proc.lock);
	/* So that a child finds no held[] or close queue being changed */
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		pthread_mutex_lock(&ls->lock);
	}
}

static void
lease_atfork_unlock_sets(void)
{
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		pthread_mutex_unlock(&ls->lock);
	}
}

static void
lease_atfork_parent(void)
{
	lease_atfork_unlock_sets();
	pthread_mutex_unlock(&lease_proc.lock);
	pthread_mutex_unlock(&lease_proc.lifecycle);
}

static void
lease_atfork_child(void)
{
	/*
	 * The child cannot use the leases, and its copies of their descriptors would keep them past
	 * the parent's exit. Closed without F_UNLCK, which would drop them for the parent too.
	 */
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		for (uint32_t i = 0; i < ls->nheld; ++i) {
			struct xal_lease_entry *e = &ls->entries[ls->held[i]];

			close(e->fd);
			e->fd = -1;
		}
		/* The caller's descriptors are the caller's to close */
		for (uint32_t i = 0; i < ls->nattached; ++i) {
			ls->entries[ls->attached[i]].fd = -1;
		}
		ls->nheld = 0;
		ls->nattached = 0;
	}
	lease_atfork_unlock_sets();
	lease_generation++;
	lease_proc.sets = NULL;
	lease_proc.nsets = 0;
	atomic_store(&lease_proc.tid, 0);
	pthread_mutex_unlock(&lease_proc.lock);
	pthread_mutex_unlock(&lease_proc.lifecycle);
}

static void
lease_atfork_register(void)
{
	lease_atfork_err =
	    pthread_atfork(lease_atfork_prepare, lease_atfork_parent, lease_atfork_child);
}

/* A set inherited across fork(), whose leases belong to the process that took them */
static bool
lease_foreign(const struct xal_lease_set *ls)
{
	return ls->generation != lease_generation;
}

struct ext_vec {
	struct xal_extent *v;
	uint32_t n;
	uint32_t cap;
};

static int
ext_vec_push(struct ext_vec *vec, const struct fiemap_extent *fe, uint32_t blocksize)
{
	if (vec->n == vec->cap) {
		uint32_t cap = vec->cap ? vec->cap * 2 : 16;
		struct xal_extent *v = realloc(vec->v, cap * sizeof(*v));

		if (!v) {
			return -ENOMEM;
		}
		vec->v = v;
		vec->cap = cap;
	}

	vec->v[vec->n].start_offset = fe->fe_logical / blocksize;
	vec->v[vec->n].start_block = fe->fe_physical / blocksize;
	vec->v[vec->n].nblocks = fe->fe_length / blocksize;
	vec->v[vec->n].flag = fiemap_extent_flag(fe->fe_flags);
	vec->n++;

	return 0;
}

/*
 * Joins extents that follow on in the file and on disk and differ at most in XAL_EXTENT_SHARED,
 * in place, and returns how many are left. XFS splits a mapping where another file shares part of
 * it, though the data has not moved, so two mappings are compared joined.
 */
static uint32_t
ext_join(struct xal_extent *v, uint32_t n)
{
	uint32_t out = 0;

	for (uint32_t i = 0; i < n; ++i) {
		struct xal_extent *prev = out ? &v[out - 1] : NULL;

		if (prev && prev->start_offset + prev->nblocks == v[i].start_offset &&
		    prev->start_block + prev->nblocks == v[i].start_block &&
		    !((prev->flag | v[i].flag) & XAL_EXTENT_NOADDR) &&
		    !((prev->flag ^ v[i].flag) & ~XAL_EXTENT_SHARED)) {
			prev->nblocks += v[i].nblocks;
			continue;
		}
		v[out++] = v[i];
	}

	return out;
}

static void
lease_close(int fd)
{
	if (fcntl(fd, F_SETLEASE, F_UNLCK)) {
		XAL_DEBUG("FAILED: F_SETLEASE(F_UNLCK); errno(%d)", errno);
	}
	close(fd);
}

/*
 * Called with ls->lock held. Forgets the lease of e. A descriptor of xal's is closed by
 * lease_unlock(), after ls->lock is released, and a break that comes meanwhile finds no entry in
 * fd_map; on the caller's descriptor the lease is released here.
 */
static void
lease_drop(struct xal_lease_set *ls, struct xal_lease_entry *e)
{
	uint32_t *list, *n, last;
	khiter_t it;

	if (e->fd < 0) {
		return;
	}

	/* The number may have gone to another entry, if the caller closed an attached descriptor */
	it = kh_get(fd_to_idx, ls->fd_map, e->fd);
	if (it != kh_end(ls->fd_map) && kh_val(ls->fd_map, it) == (uint32_t)(e - ls->entries)) {
		kh_del(fd_to_idx, ls->fd_map, it);
	}

	if (e->fd == e->ufd) {
		/* The caller's descriptor stays open; only the lease goes */
		if (fcntl(e->fd, F_SETLEASE, F_UNLCK)) {
			XAL_DEBUG("FAILED: F_SETLEASE(F_UNLCK); errno(%d)", errno);
		}
		list = ls->attached;
		n = &ls->nattached;
	} else {
		if (ls->ndead == ls->dead_cap) {
			uint32_t cap = ls->dead_cap ? ls->dead_cap * 2 : 16;
			int *dead = realloc(ls->dead, cap * sizeof(*dead));

			if (dead) {
				ls->dead = dead;
				ls->dead_cap = cap;
			}
		}
		if (ls->ndead < ls->dead_cap) {
			ls->dead[ls->ndead++] = e->fd;
		} else {
			lease_close(e->fd);
		}
		list = ls->held;
		n = &ls->nheld;
	}
	e->fd = -1;

	/* The last entry of the list takes this one's slot */
	last = list[--*n];
	list[e->slot] = last;
	ls->entries[last].slot = e->slot;
}

/* Releases ls->lock, then closes the descriptors lease_drop() set aside */
static void
lease_unlock(struct xal_lease_set *ls)
{
	uint32_t ndead = ls->ndead;
	int *dead = ls->dead;

	ls->dead = NULL;
	ls->ndead = 0;
	ls->dead_cap = 0;
	pthread_mutex_unlock(&ls->lock);

	for (uint32_t i = 0; i < ndead; ++i) {
		lease_close(dead[i]);
	}
	free(dead);
}

/*
 * Called with ls->lock held. Drops the lease of a REVOKING entry once it has no readers; the
 * last xal_extents_release() calls it again otherwise.
 */
static void
lease_finish_revoke(struct xal_lease_set *ls, uint32_t idx)
{
	struct xal_lease_entry *e = &ls->entries[idx];

	if (atomic_load(&e->state) != XAL_LEASE_REVOKING || atomic_load(&e->readers)) {
		return;
	}

	lease_drop(ls, e);
	xal_inode_at(ls->xal, idx)->content.extents.count = 0;
	atomic_store(&e->state, XAL_LEASE_REVOKED);
	ls->stats.revoked++;
}

/*
 * Called with ls->lock held. Drops the lease of one file without readers, passing over those
 * acquired since the hand last went by, as CLOCK does. Returns whether it dropped one.
 */
static bool
lease_evict_one(struct xal_lease_set *ls)
{
	for (uint32_t n = 0; n < 2 * ls->nheld; ++n) {
		uint32_t pos = ls->hand < ls->nheld ? ls->hand : 0;
		uint32_t idx = ls->held[pos];
		struct xal_lease_entry *e = &ls->entries[idx];
		int expected = XAL_LEASE_VALID;

		ls->hand = pos + 1;
		if (atomic_load(&e->state) != XAL_LEASE_VALID || atomic_load(&e->readers) ||
		    atomic_exchange(&e->ref, false)) {
			continue;
		}
		if (!atomic_compare_exchange_strong(&e->state, &expected, XAL_LEASE_EVICTING)) {
			continue;
		}
		/* Pairs with xal_extents_acquire(): a reader counted first keeps the mapping */
		if (atomic_load(&e->readers)) {
			atomic_store(&e->state, XAL_LEASE_VALID);
			continue;
		}

		/* The last held entry moves into pos, so the hand looks at it next */
		lease_drop(ls, e);
		ls->hand = pos;
		xal_inode_at(ls->xal, idx)->content.extents.count = 0;
		atomic_store(&e->state, XAL_LEASE_UNMAPPED);
		ls->stats.evicted++;

		return true;
	}

	return false;
}

/*
 * Called with ls->lock held, under which a VALID entry stays VALID. expire_ns is from
 * lease_expire_ns(), or 0 when the lease is not being broken.
 */
static void
lease_revoke(struct xal_lease_set *ls, uint32_t idx, uint64_t expire_ns)
{
	struct xal_lease_entry *e = &ls->entries[idx];
	int expected = XAL_LEASE_VALID;

	if (atomic_load(&e->state) != XAL_LEASE_VALID) {
		return;
	}
	/* Set first, so that a reader that sees REVOKING sees when it may time out */
	atomic_store(&e->expire_ns, expire_ns);

	/* Pairs with xal_extents_acquire(): readers that see REVOKING back out. */
	if (atomic_compare_exchange_strong(&e->state, &expected, XAL_LEASE_REVOKING)) {
		lease_finish_revoke(ls, idx);
	}
}

/*
 * Revoke the lease held on fd, if this set holds it. A dropped lease's fd may have been reused for
 * another file's by the time its break is taken, so the lease must be the one being broken.
 */
static bool
lease_break(struct xal_lease_set *ls, int fd, uint64_t expire_ns)
{
	khiter_t it;
	bool found;

	pthread_mutex_lock(&ls->lock);
	it = kh_get(fd_to_idx, ls->fd_map, fd);
	found = it != kh_end(ls->fd_map);
	if (found && fcntl(fd, F_GETLEASE) != ls->entries[kh_val(ls->fd_map, it)].ltype) {
		ls->stats.breaks++;
		lease_revoke(ls, kh_val(ls->fd_map, it), expire_ns);
	}
	lease_unlock(ls);

	return found;
}

/* The kernel sends SIGIO when it cannot queue a break's signal, so about when it broke */
static void
lease_rescan(struct xal_lease_set *ls, uint64_t expire_ns)
{
	pthread_mutex_lock(&ls->lock);
	ls->stats.rescans++;

	/* Backwards: dropping an entry moves the last of its list into its slot */
	for (int attached = 0; attached < 2; ++attached) {
		uint32_t *list = attached ? ls->attached : ls->held;

		for (uint32_t i = attached ? ls->nattached : ls->nheld; i-- > 0;) {
			uint32_t idx = list[i];
			struct xal_lease_entry *e = &ls->entries[idx];

			/* A lease being broken reports the type it is breaking to, not its own */
			if (atomic_load(&e->state) == XAL_LEASE_VALID &&
			    fcntl(e->fd, F_GETLEASE) != e->ltype) {
				lease_revoke(ls, idx, expire_ns);
			}
		}
	}

	lease_unlock(ls);
}

/*
 * Hand fd to the set holding its lease, or with fd -1 have every set check all its leases. Holding
 * lease_proc.lock keeps the sets from being closed meanwhile; nothing under it waits for a reader.
 */
static void
lease_dispatch(int fd)
{
	uint64_t expire_ns = lease_expire_ns();
	struct xal_lease_set *ls;

	pthread_mutex_lock(&lease_proc.lock);
	for (ls = lease_proc.sets; ls; ls = ls->next) {
		if (fd < 0) {
			lease_rescan(ls, expire_ns);
		} else if (lease_break(ls, fd, expire_ns)) {
			break;
		}
	}
	pthread_mutex_unlock(&lease_proc.lock);
}

/*
 * Every signal is blocked in this thread from its creation. The realtime signal carries the fd of
 * a broken lease; SIGIO is what the kernel sends instead when it cannot queue that signal.
 */
static void *
lease_thread(void *arg)
{
	struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
	sigset_t set;

	(void)arg;

	sigemptyset(&set);
	sigaddset(&set, lease_proc.sig);
	sigaddset(&set, SIGIO);
	atomic_store(&lease_proc.tid, gettid());

	while (!atomic_load(&lease_proc.stop)) {
		siginfo_t si;
		int sig = sigtimedwait(&set, &si, &timeout);

		/* A lease break comes as POLL_MSG; anything else on the signal is not ours */
		if (sig == lease_proc.sig && si.si_code == POLL_MSG) {
			lease_dispatch(si.si_fd);
		} else if (sig == SIGIO) {
			lease_dispatch(-1);
		}
	}

	return NULL;
}

/* Called with lease_proc.lifecycle held */
static int
lease_proc_start(void)
{
	sigset_t all, orig;
	int err;

	lease_proc.sig = SIGRTMIN + XAL_LEASE_SIGOFF;
	atomic_store(&lease_proc.tid, 0);
	atomic_store(&lease_proc.stop, false);

	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &orig);
	err = -pthread_create(&lease_proc.thread, NULL, lease_thread, NULL);
	pthread_sigmask(SIG_SETMASK, &orig, NULL);
	if (err) {
		return err;
	}

	/* Leases are taken with the thread as owner, so wait until it is known */
	while (!atomic_load(&lease_proc.tid)) {
		nanosleep(&lease_nap, NULL);
	}

	return 0;
}

/*
 * Called with lease_proc.lifecycle held. Signals sent to the thread die with it, so leases still
 * held by a set being closed go unreported until it drops them.
 */
static void
lease_proc_stop(void)
{
	atomic_store(&lease_proc.stop, true);
	/* Woken now rather than at its next timeout; a signal not from a break is ignored there */
	pthread_kill(lease_proc.thread, lease_proc.sig);
	pthread_join(lease_proc.thread, NULL);
}

static int
lease_proc_register(struct xal_lease_set *ls)
{
	int err = 0;

	pthread_once(&lease_atfork_once, lease_atfork_register);
	if (lease_atfork_err) {
		return -lease_atfork_err;
	}

	pthread_mutex_lock(&lease_proc.lifecycle);
	if (!lease_proc.nsets) {
		err = lease_proc_start();
	}
	if (!err) {
		pthread_mutex_lock(&lease_proc.lock);
		ls->next = lease_proc.sets;
		lease_proc.sets = ls;
		lease_proc.nsets++;
		pthread_mutex_unlock(&lease_proc.lock);
	}
	pthread_mutex_unlock(&lease_proc.lifecycle);

	return err;
}

static void
lease_proc_unregister(struct xal_lease_set *ls)
{
	struct xal_lease_set **pp;
	bool found = false;
	unsigned nsets;

	pthread_mutex_lock(&lease_proc.lifecycle);

	pthread_mutex_lock(&lease_proc.lock);
	for (pp = &lease_proc.sets; *pp; pp = &(*pp)->next) {
		if (*pp == ls) {
			*pp = ls->next;
			lease_proc.nsets--;
			found = true;
			break;
		}
	}
	nsets = lease_proc.nsets;
	pthread_mutex_unlock(&lease_proc.lock);

	/* A set inherited across fork() is in no list here, and the thread is not this process's */
	if (found && !nsets) {
		lease_proc_stop();
	}

	pthread_mutex_unlock(&lease_proc.lifecycle);
}

/*
 * With sync, the first call flushes as read_fiemap() does, see fiemap_ioctl(). Extents past EOF,
 * such as speculative preallocation, are left out or cut at the block holding EOF.
 */
static int
map_fiemap(int fd, uint64_t size, uint32_t blocksize, bool sync, struct ext_vec *vec)
{
	struct fiemap *fm;
	uint64_t start = 0;
	int err = 0;

	/* The kernel reads only the header, and every release with recheck calls this */
	fm = malloc(sizeof(*fm) + XAL_FIEMAP_BATCH * sizeof(struct fiemap_extent));
	if (!fm) {
		return -ENOMEM;
	}
	memset(fm, 0, sizeof(*fm));

	while (start < size) {
		struct fiemap_extent *last;

		fm->fm_start = start;
		fm->fm_length = size - start;
		fm->fm_extent_count = XAL_FIEMAP_BATCH;
		fm->fm_mapped_extents = 0;

		err = fiemap_ioctl(fd, fm, sync && start == 0);
		if (err) {
			break;
		}

		for (uint32_t i = 0; i < fm->fm_mapped_extents && !err; ++i) {
			struct fiemap_extent *fe = &fm->fm_extents[i];

			if (fe->fe_logical >= size) {
				continue;
			}
			if (fe->fe_length > size - fe->fe_logical) {
				uint64_t len = size - fe->fe_logical;

				fe->fe_length = (len + blocksize - 1) / blocksize * blocksize;
			}
			err = ext_vec_push(vec, fe, blocksize);
		}
		if (err || !fm->fm_mapped_extents) {
			break;
		}

		last = &fm->fm_extents[fm->fm_mapped_extents - 1];
		if (last->fe_flags & FIEMAP_EXTENT_LAST) {
			break;
		}
		start = last->fe_logical + last->fe_length;
	}

	free(fm);

	return err;
}

/* Opens the file read-only by its path, for a lease on a descriptor of xal's; fd or -errno */
static int
lease_open_path(struct xal_lease_set *ls, struct xal_inode *inode)
{
	char path[XAL_INODE_PATH_MAXLEN + 1];
	int fd, err;

	err = xal_inode_path(ls->xal, inode, path, sizeof(path));
	if (err < 0) {
		return err;
	}

	pthread_mutex_lock(&ls->lock);
	if (ls->nheld >= ls->max_held) {
		lease_evict_one(ls);
	}
	lease_unlock(ls);

	for (;;) {
		bool evicted;

		/* O_NONBLOCK: a write lease held by someone else fails the open, not blocks it */
		fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			return fd;
		}
		err = -errno;
		if (err != -EMFILE && err != -ENFILE) {
			return (err == -ENOENT || err == -ELOOP) ? -ESTALE : err;
		}

		pthread_mutex_lock(&ls->lock);
		evicted = lease_evict_one(ls);
		lease_unlock(ls);
		if (!evicted) {
			return err;
		}
	}
}

/*
 * Called with the entry in MAPPING, which keeps other callers off it and readers off its extents.
 * Takes ls->lock only to publish, so a slow writeback holds up no lease break.
 */
static int
lease_and_map(struct xal_lease_set *ls, struct xal_inode *inode, uint32_t idx)
{
	struct xal_lease_entry *e = &ls->entries[idx];
	struct f_owner_ex owner = {.type = F_OWNER_TID, .pid = atomic_load(&lease_proc.tid)};
	uint32_t extent_idx, *n, *cap, **list;
	bool attached = e->ufd >= 0;
	struct ext_vec vec = {0};
	struct stat st;
	khiter_t it;
	int fd, ret, err;

	if (attached) {
		fd = e->ufd;
	} else {
		e->ltype = F_RDLCK;
		fd = lease_open_path(ls, inode);
		if (fd < 0) {
			return fd;
		}
	}

	if (fstat(fd, &st)) {
		err = -errno;
		goto failed;
	}
	/* The path may now lead elsewhere, even onto another fs with the same inode number */
	if (!S_ISREG(st.st_mode) || st.st_ino != inode->ino || st.st_dev != ls->dev) {
		err = -ESTALE;
		goto failed;
	}

	if (fcntl(fd, F_SETSIG, lease_proc.sig) || fcntl(fd, F_SETOWN_EX, &owner)) {
		err = -errno;
		goto failed;
	}
	if (fcntl(fd, F_SETLEASE, e->ltype)) {
		err = -errno;
		if (err == -EAGAIN) {
			pthread_mutex_lock(&ls->lock);
			ls->stats.refused++;
			lease_unlock(ls);
		}
		goto failed;
	}

	/*
	 * No other open() for writing or truncate() can happen now without breaking the lease, so
	 * size and mapping hold, except for writes through the descriptor of a write lease itself,
	 * which lease_check() finds by size and ctime. The ctime is taken before the FIEMAP's sync:
	 * on XFS a buffered write holds the IO lock from its ctime update to the end of its copy
	 * and the FIEMAP takes it around the sync, so such a write is either written back by the
	 * sync or has a later ctime. An O_DIRECT write holds that lock only shared, and one in
	 * flight across the mapping may change the extents after it with no later ctime.
	 */
	if (fstat(fd, &st)) {
		err = -errno;
		goto failed_unlock;
	}
	e->ctime = st.st_ctim;

	err = map_fiemap(fd, st.st_size, ls->xal->sb.blocksize, true, &vec);
	if (err) {
		goto failed_unlock;
	}

	pthread_mutex_lock(&ls->lock);

	list = attached ? &ls->attached : &ls->held;
	n = attached ? &ls->nattached : &ls->nheld;
	cap = attached ? &ls->attached_cap : &ls->held_cap;
	if (*n == *cap) {
		uint32_t grown_cap = *cap ? *cap * 2 : 64;
		uint32_t *grown = realloc(*list, grown_cap * sizeof(*grown));

		if (!grown) {
			lease_unlock(ls);
			err = -ENOMEM;
			goto failed_unlock;
		}
		*list = grown;
		*cap = grown_cap;
	}

	/* No reader is on the old extents, so their region is reused if it is large enough */
	extent_idx = inode->content.extents.extent_idx;
	if (vec.n > e->extent_cap) {
		uint32_t cap = 1;

		/* Rounded up, as a file mapped again after an eviction may have grown */
		while (cap < vec.n) {
			cap <<= 1;
		}
		err = xal_pool_claim_extents(&ls->xal->extents, cap, &extent_idx);
		if (err) {
			lease_unlock(ls);
			goto failed_unlock;
		}
		/* Together, so that a mapping failing below leaves the cap with its region */
		inode->content.extents.extent_idx = extent_idx;
		e->extent_cap = cap;
	}

	it = kh_put(fd_to_idx, ls->fd_map, fd, &ret);
	if (ret < 0) {
		lease_unlock(ls);
		err = -ENOMEM;
		goto failed_unlock;
	}
	kh_val(ls->fd_map, it) = idx;

	/*
	 * A break before the fd was in fd_map found nothing to revoke, and a lease the kernel timed
	 * out while mapping is gone; F_GETLEASE then shows another type than the one taken. A later
	 * break finds the fd once ls->lock is released.
	 */
	if (fcntl(fd, F_GETLEASE) != e->ltype) {
		kh_del(fd_to_idx, ls->fd_map, it);
		lease_unlock(ls);
		err = -EAGAIN;
		goto failed_unlock;
	}

	if (vec.n) {
		memcpy(xal_extent_at(ls->xal, extent_idx), vec.v, vec.n * sizeof(*vec.v));
	}
	inode->content.extents.extent_idx = extent_idx;
	inode->content.extents.count = vec.n;
	inode->size = st.st_size;

	e->fd = fd;
	e->slot = *n;
	(*list)[(*n)++] = idx;
	atomic_store(&e->expire_ns, 0);
	atomic_store(&e->stale, false);
	ls->stats.mapped++;
	atomic_store(&e->state, XAL_LEASE_VALID);

	lease_unlock(ls);

	free(vec.v);

	return 0;

failed_unlock:
	fcntl(fd, F_SETLEASE, F_UNLCK);
failed:
	free(vec.v);
	if (attached) {
		struct f_owner_ex none = {.type = F_OWNER_PID, .pid = 0};

		/* F_UNLCK clears these; without a lease taken, they are cleared here */
		fcntl(fd, F_SETSIG, 0);
		fcntl(fd, F_SETOWN_EX, &none);
	} else {
		close(fd);
	}

	return err;
}

static struct xal_lease_set *
lease_set(struct xal *xal)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;

	return be->base.type == XAL_BACKEND_FIEMAP ? be->lease : NULL;
}

/* Count one reader out; the last reader of a REVOKING entry drops its lease */
static int
lease_reader_put(struct xal_lease_set *ls, uint32_t idx)
{
	struct xal_lease_entry *e = &ls->entries[idx];
	unsigned readers = atomic_load(&e->readers);

	do {
		if (!readers) {
			XAL_DEBUG("FAILED: release without acquire; inode idx(%u)", idx);
			return -EINVAL;
		}
	} while (!atomic_compare_exchange_weak(&e->readers, &readers, readers - 1));

	if (readers == 1 && atomic_load(&e->state) == XAL_LEASE_REVOKING) {
		pthread_mutex_lock(&ls->lock);
		lease_finish_revoke(ls, idx);
		lease_unlock(ls);
	}

	return 0;
}

/*
 * Whether the kernel still holds the lease on fd: 1 if so, 0 if not, negative errno if it cannot
 * tell. A broken lease stays listed, as BREAKING, until the kernel lets the breaker through.
 */
static int
lease_held(struct xal_lease_set *ls, int fd)
{
	char path[64], line[256];
	bool held = false;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	for (;;) {
		int err;
		bool evicted;

		f = fopen(path, "re");
		if (f) {
			break;
		}
		err = -errno;
		if (err != -EMFILE && err != -ENFILE) {
			return err;
		}

		/* With every descriptor taken, as past lease_max, the clock would decide instead */
		pthread_mutex_lock(&ls->lock);
		evicted = lease_evict_one(ls);
		lease_unlock(ls);
		if (!evicted) {
			return err;
		}
	}
	while (!held && fgets(line, sizeof(line), f)) {
		held = !strncmp(line, "lock:", 5) && strstr(line, " LEASE ");
	}
	fclose(f);

	return held;
}

/*
 * Whether the extents handed to a reader may no longer hold the file's data: a release already
 * found so, or the kernel has let a writer past the broken lease, or, with recheck, the file no
 * longer maps as it did, as after a dedupe into it or the filesystem moving its data, which break
 * no lease. Called by a reader, which keeps e->fd open and the extents in place.
 */
static int
lease_check(struct xal_lease_set *ls, struct xal_inode *inode, struct xal_lease_entry *e)
{
	struct xal_extents *extents = &inode->content.extents;
	struct ext_vec vec = {0};
	uint64_t expire_ns;
	struct xal_extent *old = NULL;
	uint32_t nold;
	int err;

	if (atomic_load(&e->stale)) {
		return -ESTALE;
	}
	if (atomic_load(&e->state) == XAL_LEASE_REVOKING) {
		err = lease_held(ls, e->fd);
		expire_ns = atomic_load(&e->expire_ns);
		if (!err || (err < 0 && expire_ns && now_ns() >= expire_ns)) {
			return -ESTALE;
		}
	}
	/* The holder of a write lease writes without breaking it; size and ctime show it */
	if (e->ltype == F_WRLCK) {
		struct stat st;

		if (fstat(e->fd, &st)) {
			return -errno;
		}
		if ((uint64_t)st.st_size != inode->size || st.st_ctim.tv_sec != e->ctime.tv_sec ||
		    st.st_ctim.tv_nsec != e->ctime.tv_nsec) {
			return -ESTALE;
		}
	}
	if (!ls->recheck) {
		return 0;
	}

	err = map_fiemap(e->fd, inode->size, ls->xal->sb.blocksize, false, &vec);
	if (!err) {
		old = malloc((extents->count ? extents->count : 1) * sizeof(*old));
		err = old ? 0 : -ENOMEM;
	}
	for (uint32_t i = 0; i < extents->count && !err; ++i) {
		old[i] = *xal_extent_at(ls->xal, extents->extent_idx + i);
	}
	if (!err) {
		nold = ext_join(old, extents->count);
		vec.n = ext_join(vec.v, vec.n);
		err = nold == vec.n ? 0 : -ESTALE;
	}
	for (uint32_t i = 0; i < vec.n && !err; ++i) {
		/* Sharing changes with other files, as after a dedupe into one of them */
		if (old[i].start_offset != vec.v[i].start_offset ||
		    old[i].start_block != vec.v[i].start_block ||
		    old[i].nblocks != vec.v[i].nblocks ||
		    ((old[i].flag ^ vec.v[i].flag) & ~XAL_EXTENT_SHARED)) {
			err = -ESTALE;
		}
	}
	free(old);
	free(vec.v);

	return err;
}

int
xal_extents_acquire(struct xal *xal, struct xal_inode *inode, struct xal_extents **extents)
{
	struct timespec nap = lease_nap;
	struct xal_lease_set *ls;
	struct xal_lease_entry *e;
	uint32_t idx;

	if (!xal || !inode || !extents || !xal_inode_is_file(inode)) {
		return -EINVAL;
	}

	ls = lease_set(xal);
	if (!ls) {
		*extents = &inode->content.extents;
		return 0;
	}
	if (lease_foreign(ls)) {
		return -ECHILD;
	}

	idx = xal_inode_idx(xal, inode);
	if (idx >= ls->nentries) {
		return -EINVAL;
	}
	e = &ls->entries[idx];

	for (;;) {
		int state, err;

		/* Pairs with lease_revoke(), which sets REVOKING, then drops at 0 readers */
		atomic_fetch_add(&e->readers, 1);
		if (atomic_load(&e->state) == XAL_LEASE_VALID) {
			atomic_store(&e->ref, true);
			*extents = &inode->content.extents;
			return 0;
		}
		lease_reader_put(ls, idx);

		state = atomic_load(&e->state);
		switch (state) {
		case XAL_LEASE_VALID:
			continue;
		case XAL_LEASE_REVOKING:
			return -EAGAIN;
		case XAL_LEASE_MAPPING:
			nanosleep(&nap, NULL);
			nap.tv_nsec = nap.tv_nsec * 2 < XAL_LEASE_NAP_MAX_NS ? nap.tv_nsec * 2
									     : XAL_LEASE_NAP_MAX_NS;
			continue;
		case XAL_LEASE_EVICTING:
			/* Eviction only drops a lease, unlike a mapping */
			nanosleep(&lease_nap, NULL);
			continue;
		default:
			break;
		}

		if (!atomic_compare_exchange_strong(&e->state, &state, XAL_LEASE_MAPPING)) {
			continue;
		}
		err = lease_and_map(ls, inode, idx);
		if (err == -EAGAIN && atomic_load(&e->state) == XAL_LEASE_MAPPING) {
			atomic_store(&e->state, XAL_LEASE_REFUSED);
		} else if (err && atomic_load(&e->state) == XAL_LEASE_MAPPING) {
			atomic_store(&e->state, state);
		}
		if (err) {
			return err;
		}
	}
}

int
xal_extents_release(struct xal *xal, struct xal_inode *inode)
{
	struct xal_lease_set *ls;
	uint32_t idx;
	int err, put;

	if (!xal || !inode) {
		return -EINVAL;
	}

	ls = lease_set(xal);
	if (!ls) {
		return 0;
	}
	if (lease_foreign(ls)) {
		return -ECHILD;
	}

	idx = xal_inode_idx(xal, inode);
	if (idx >= ls->nentries) {
		return -EINVAL;
	}
	if (!atomic_load(&ls->entries[idx].readers)) {
		XAL_DEBUG("FAILED: release without acquire; inode idx(%u)", idx);
		return -EINVAL;
	}

	err = lease_check(ls, inode, &ls->entries[idx]);
	if (err) {
		pthread_mutex_lock(&ls->lock);
		if (err == -ESTALE) {
			ls->stats.stale++;
		}
		/* Other readers are told too, and the next acquire maps the file again */
		atomic_store(&ls->entries[idx].stale, true);
		lease_revoke(ls, idx, 0);
		lease_unlock(ls);
	}

	put = lease_reader_put(ls, idx);

	return put ? put : err;
}

/* The entry of a regular file in a set of this process, or NULL with *err set */
static struct xal_lease_entry *
lease_entry(struct xal *xal, struct xal_inode *inode, struct xal_lease_set **ls, uint32_t *idx,
	    int *err)
{
	*err = -EINVAL;
	if (!xal || !inode || !xal_inode_is_file(inode)) {
		return NULL;
	}
	*ls = lease_set(xal);
	if (!*ls) {
		return NULL;
	}
	if (lease_foreign(*ls)) {
		*err = -ECHILD;
		return NULL;
	}
	*idx = xal_inode_idx(xal, inode);
	if (*idx >= (*ls)->nentries) {
		return NULL;
	}
	*err = 0;

	return &(*ls)->entries[*idx];
}

/* Called with ls->lock held. Forgets the caller's descriptor of e. */
static void
lease_forget_ufd(struct xal_lease_set *ls, struct xal_lease_entry *e)
{
	khiter_t it = kh_get(fd_to_idx, ls->ufd_map, e->ufd);

	if (it != kh_end(ls->ufd_map)) {
		kh_del(fd_to_idx, ls->ufd_map, it);
	}
	e->ufd = -1;
}

/*
 * Called with ls->lock held. Moves an entry without readers from its state to "to", MAPPING or
 * EVICTING, saving the old state; -EBUSY when it has readers or is being mapped, evicted or
 * revoked.
 */
static int
lease_claim(struct xal_lease_entry *e, int to, int *state)
{
	*state = atomic_load(&e->state);
	if (atomic_load(&e->readers) || *state == XAL_LEASE_MAPPING ||
	    *state == XAL_LEASE_EVICTING || *state == XAL_LEASE_REVOKING) {
		return -EBUSY;
	}
	if (!atomic_compare_exchange_strong(&e->state, state, to)) {
		return -EBUSY;
	}
	/* Pairs with xal_extents_acquire(), as in lease_evict_one() */
	if (atomic_load(&e->readers)) {
		atomic_store(&e->state, *state);
		return -EBUSY;
	}

	return 0;
}

static int
lease_attach(struct xal *xal, struct xal_inode *inode, int fd)
{
	struct xal_lease_entry *e;
	struct xal_lease_set *ls;
	struct stat st;
	uint32_t idx;
	int flags, state, ret, err;

	e = lease_entry(xal, inode, &ls, &idx, &err);
	if (!e) {
		return err;
	}
	if (fd < 0) {
		return -EINVAL;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fstat(fd, &st)) {
		return -errno;
	}
	if (!S_ISREG(st.st_mode) || st.st_ino != inode->ino || st.st_dev != ls->dev) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ls->lock);
	/* An open file attached twice would get its breaks under one entry only */
	if (e->ufd >= 0 || kh_get(fd_to_idx, ls->ufd_map, fd) != kh_end(ls->ufd_map) ||
	    kh_get(fd_to_idx, ls->fd_map, fd) != kh_end(ls->fd_map)) {
		err = -EBUSY;
	} else {
		err = lease_claim(e, XAL_LEASE_MAPPING, &state);
	}
	if (!err) {
		khiter_t it = kh_put(fd_to_idx, ls->ufd_map, fd, &ret);

		if (ret < 0) {
			atomic_store(&e->state, state);
			err = -ENOMEM;
		} else {
			kh_val(ls->ufd_map, it) = idx;
		}
	}
	if (err) {
		lease_unlock(ls);
		return err;
	}
	/* A lease on a descriptor of xal's gives way */
	lease_drop(ls, e);
	xal_inode_at(xal, idx)->content.extents.count = 0;
	e->ufd = fd;
	e->ltype = (flags & O_ACCMODE) == O_RDONLY ? F_RDLCK : F_WRLCK;
	lease_unlock(ls);

	err = lease_and_map(ls, inode, idx);
	if (err) {
		pthread_mutex_lock(&ls->lock);
		lease_forget_ufd(ls, e);
		atomic_store(&e->state, err == -EAGAIN ? XAL_LEASE_REFUSED : XAL_LEASE_UNMAPPED);
		lease_unlock(ls);
	}

	return err;
}

int
xal_extents_attach_fd(struct xal *xal, struct xal_inode *inode, int fd, struct xal_inode **attached)
{
	int err;

	err = lease_attach(xal, inode, fd);
	if (!err && attached) {
		*attached = inode;
	}

	return err;
}

int
xal_extents_detach_fd(struct xal *xal, struct xal_inode *inode)
{
	struct xal_lease_entry *e;
	struct xal_lease_set *ls;
	uint32_t idx;
	int state, err;

	e = lease_entry(xal, inode, &ls, &idx, &err);
	if (!e) {
		return err;
	}

	pthread_mutex_lock(&ls->lock);
	err = e->ufd < 0 ? -EINVAL : lease_claim(e, XAL_LEASE_EVICTING, &state);
	if (!err) {
		lease_drop(ls, e);
		xal_inode_at(xal, idx)->content.extents.count = 0;
		lease_forget_ufd(ls, e);
		atomic_store(&e->state, XAL_LEASE_UNMAPPED);
	}
	lease_unlock(ls);

	return err;
}

int
xal_lease_stats(struct xal *xal, struct xal_lease_stats *stats)
{
	struct xal_lease_set *ls;

	if (!xal || !stats) {
		return -EINVAL;
	}

	ls = lease_set(xal);
	if (!ls) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ls->lock);
	*stats = ls->stats;
	lease_unlock(ls);

	return 0;
}

int
xal_be_fiemap_lease_reset(struct xal *xal, bool force)
{
	struct xal_lease_set *ls = lease_set(xal);

	if (!ls) {
		return 0;
	}

	/*
	 * lease_atfork_child() closed the descriptors of every set listed at fork(). What is still
	 * open here, in the entries or in dead[], belongs to a set that another thread was closing
	 * then; it is closed without F_UNLCK, which would drop the parent's leases, and without
	 * ls->lock, which that thread may have held.
	 */
	if (lease_foreign(ls)) {
		if (!force) {
			return -ECHILD;
		}
		for (uint32_t i = 0; i < ls->nentries; ++i) {
			if (ls->entries[i].fd >= 0) {
				if (ls->entries[i].fd != ls->entries[i].ufd) {
					close(ls->entries[i].fd);
				}
				ls->entries[i].fd = -1;
			}
		}
		for (uint32_t i = 0; i < ls->ndead; ++i) {
			close(ls->dead[i]);
		}
		free(ls->dead);
		ls->dead = NULL;
		ls->ndead = 0;
		ls->dead_cap = 0;
		return 0;
	}

	pthread_mutex_lock(&ls->lock);
	for (uint32_t i = 0; i < ls->nentries && !force; ++i) {
		if (atomic_load(&ls->entries[i].readers)) {
			lease_unlock(ls);
			XAL_DEBUG("FAILED: inode idx(%u) has readers", i);
			return -EBUSY;
		}
	}
	for (uint32_t i = 0; i < ls->nentries; ++i) {
		struct xal_lease_entry *e = &ls->entries[i];

		lease_drop(ls, e);
		e->ufd = -1;
		atomic_store(&e->readers, 0);
		atomic_store(&e->state, XAL_LEASE_UNMAPPED);
		e->extent_cap = 0;
	}
	kh_clear(fd_to_idx, ls->fd_map);
	kh_clear(fd_to_idx, ls->ufd_map);
	lease_unlock(ls);

	return 0;
}

int
xal_be_fiemap_lease_prepare(struct xal *xal)
{
	struct xal_lease_set *ls = lease_set(xal);
	struct xal_lease_entry *entries;
	size_t n;

	if (!ls) {
		return 0;
	}

	n = xal->inodes.free;
	entries = calloc(n ? n : 1, sizeof(*entries));
	if (!entries) {
		return -ENOMEM;
	}
	for (size_t i = 0; i < n; ++i) {
		entries[i].fd = -1;
		entries[i].ufd = -1;
	}

	pthread_mutex_lock(&ls->lock);
	free(ls->entries);
	ls->entries = entries;
	ls->nentries = n;
	ls->hand = 0;
	lease_unlock(ls);

	return 0;
}

int
xal_be_fiemap_lease_init(struct xal *xal, const char *mountpoint, const struct xal_opts *opts)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct xal_lease_set *ls;
	struct rlimit rl;
	struct stat st;
	int err;

	if (stat(mountpoint, &st)) {
		return -errno;
	}

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		return -ENOMEM;
	}
	ls->xal = xal;
	ls->dev = st.st_dev;
	ls->generation = lease_generation;
	ls->recheck = !opts->lease_no_recheck;
	lease_break_time();
	ls->max_held = opts->lease_max;
	if (!ls->max_held) {
		ls->max_held = UINT32_MAX;
		if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur != RLIM_INFINITY &&
		    rl.rlim_cur / 2 < UINT32_MAX) {
			ls->max_held = rl.rlim_cur > 1 ? rl.rlim_cur / 2 : 1;
		}
	}
	pthread_mutex_init(&ls->lock, NULL);

	ls->fd_map = kh_init(fd_to_idx);
	ls->ufd_map = kh_init(fd_to_idx);
	if (!ls->fd_map || !ls->ufd_map) {
		err = -ENOMEM;
		goto failed;
	}

	err = lease_proc_register(ls);
	if (err) {
		goto failed;
	}

	be->lease = ls;

	XAL_DEBUG("INFO: read-lease mode; signal(%d) to tid(%d), max_held(%u)", lease_proc.sig,
		  atomic_load(&lease_proc.tid), ls->max_held);

	return 0;

failed:
	if (ls->fd_map) {
		kh_destroy(fd_to_idx, ls->fd_map);
	}
	if (ls->ufd_map) {
		kh_destroy(fd_to_idx, ls->ufd_map);
	}
	pthread_mutex_destroy(&ls->lock);
	free(ls);

	return err;
}

void
xal_be_fiemap_lease_close(struct xal *xal)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct xal_lease_set *ls = be->lease;

	if (!ls) {
		return;
	}

	lease_proc_unregister(ls);
	xal_be_fiemap_lease_reset(xal, true);

	kh_destroy(fd_to_idx, ls->fd_map);
	kh_destroy(fd_to_idx, ls->ufd_map);
	free(ls->entries);
	free(ls->held);
	free(ls->attached);
	pthread_mutex_destroy(&ls->lock);
	free(ls);
	be->lease = NULL;
}
