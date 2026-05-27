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
and ``mmap(MAP_SHARED)``; there is no lazy growth. The objects persist in the
shared memory filesystem (``/dev/shm`` on Linux) until explicitly removed.

## Consumer processes: ``xal_from_shm()``

A secondary process that needs read-only access to an already-indexed pool can
attach to the shared memory objects directly, without opening the device or
re-running ``xal_index()``. All metadata (superblock, backend type, mountpoint,
root inode index) is read from a dedicated ``_state`` shared memory region
created by the primary; no out-of-band communication is needed beyond the base
shared memory name.

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
