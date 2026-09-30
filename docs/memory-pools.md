# Pool Memory

Inodes and extents are each stored in a separate pool (``struct xal_pool``),
backed by a large over-committed ``mmap`` region.

The pool reserves a virtual address range upfront sized for the maximum
expected number of elements, but only commits physical pages in chunks as
elements are claimed (via ``mprotect``). This keeps the array contiguous in
memory — ``xal_inode_at(xal, idx)`` is a plain pointer offset — and means
elements never move, so pool indices remain stable across all insertions.

## Lazy growth (anonymous mode)

By default, pools use private anonymous memory. The full virtual address range
is reserved with ``PROT_NONE`` at open time; physical pages are committed in
chunks of ``growby`` elements by calling ``mprotect(PROT_READ|PROT_WRITE)``
whenever the pool runs low. This avoids upfront memory commitment while keeping
the array at a single contiguous address.

## Shared memory mode

When ``xal_opts.shm_name`` is set, the two pools are backed by POSIX shared
memory objects instead of anonymous memory. Because all internal
cross-references within the pools use integer indices rather than raw
pointers, the pool data is valid regardless of the virtual address at which
it is mapped in each process. The names of the objects are
derived from the base name by appending ``_inodes`` and ``_extents``
respectively::

   opts.shm_name = "/myapp_xal";
   /* creates /myapp_xal_inodes and /myapp_xal_extents */

In this mode the full reserved size is committed upfront via ``ftruncate()``
and ``mmap(MAP_SHARED)``; there is no lazy growth. The objects live in the
shared memory filesystem (``/dev/shm`` on Linux) and are removed by the
creating process on ``xal_close()``; see "Process roles" below.

## Consumer processes: ``xal_from_shm()``

A secondary process that needs read-only access to an already-indexed pool can
attach to the shared memory objects directly, without opening the device or
re-running ``xal_index()``. All metadata (superblock, backend type, mountpoint,
the subtree a scoped index is rooted at, root inode index) is read from a
dedicated ``_state`` shared memory region created by the primary; no
out-of-band communication is needed beyond the base shared memory name.

The typical pattern is:

1. One process calls ``xal_open()`` with ``shm_name`` set and runs
   ``xal_index()``. The shared memory name must be communicated to the
   secondary process, for example through a command-line argument or
   environment variable.

2. The secondary calls ``xal_from_shm()`` with that name to obtain a
   read-only ``struct xal *``::

      const char *shm_name = /* shared memory base name */;
      struct xal *view;

      xal_from_shm(shm_name, &view);
      xal_walk(view, xal_get_root(view), my_callback, NULL);
      xal_close(view);

## Process roles

Every ``struct xal`` carries one of three roles, recorded internally as
``enum xal_procrole``:

``XAL_PROCROLE_SINGLE``
   The default. ``xal_open()`` was called without ``shm_name``, so the pools
   are private anonymous memory and no other process is involved.

``XAL_PROCROLE_PRIMARY``
   ``xal_open()`` was called with ``shm_name`` set. This handle owns the
   shared memory objects: it created them, it is the only one allowed to
   index into them, and it removes them again.

``XAL_PROCROLE_SECONDARY``
   The handle came from ``xal_from_shm()``. It maps the objects read-only and
   owns nothing.

The role decides two things:

**Ownership at close.** ``xal_close()`` always unmaps the pools, but only a
primary (or single) handle also ``shm_unlink()``s the ``_inodes``,
``_extents`` and ``_state`` objects. A secondary detaches without removing
anything, so several secondaries may attach and close independently. The
consequence is that the shared memory objects live as long as the primary
does: once the primary closes, the names are gone and no new secondary can
attach, even though already-attached secondaries keep their mapping valid
until they close.

A primary marks the index dirty before it tears the backend down, so that a
secondary attaching during the teardown gets ``-ESTALE`` rather than a handle
describing a snapshot that is being deleted. The mapping an already-attached
secondary holds stays readable regardless — ``shm_unlink()`` removes the name,
not the object — but ``xal_get_inode()``, ``xal_get_extents()``,
``xal_get_dentries()``, ``xal_build_lookup_hashmap()`` and ``xal_walk()`` all
check the flag and start refusing. ``xal_get_root()`` and ``xal_inode_at()``
return pointers and cannot report it, and no check is repeated once a walk is
under way: the primary may close at any point between a check and the use of
what it validated. Ordering the mark first narrows that window; it does not
remove it.

**Indexing.** ``xal_index()`` and ``xal_dinodes_retrieve()`` both return
``-EINVAL`` on a secondary handle. The pools are mapped read-only there, and
the index is the primary's to build and rebuild. ``xal_dinodes_retrieve()`` is
refused on both backends, not only on XFS where it reads the device: a
secondary owns no device relationship, and a caller reaching it has put a
primary-only step outside the role check rather than inside it, which is worth
being told about whichever backend it happens under.

A secondary that finds the view stale — attaching returns ``-ESTALE`` when the
region has been marked dirty — must wait for the primary to re-index rather
than re-index itself.

The state region begins with a magic and a version, and ``xal_from_shm()``
checks both, along with the size of the region. ``-EPROTO`` says the primary
published from a build this one cannot read; unlike ``-ESTALE``, ``-EAGAIN``
and ``-ENOENT``, it never resolves by waiting. The version covers what the
pools mean, not only how they are laid out, so a change to the interpretation
of a field requires a bump even though every size and offset is unchanged;
``xal_inode.name`` holding a leaf name rather than an assembled path is one.
``-EAGAIN`` marks the two windows where a primary has created the region but
not finished publishing it: the magic is stored last, after the region is
sized and every other field is written. A primary that died inside one of
those windows leaves a region that answers ``-EAGAIN`` for good, and the name
has to be unlinked before another primary can take it.
