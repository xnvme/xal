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
The process that opened xal with ``shm_name`` set is responsible for calling
``shm_unlink()`` on both objects when they are no longer needed.
``xal_close()`` will ``munmap`` the regions but will not unlink them.

## Consumer processes: ``xal_from_shm()``

A secondary process that needs read-only access to an already-indexed pool can
attach to the shared memory objects directly, without opening the device or
re-running ``xal_index()``. The typical pattern is:

1. One process calls ``xal_open()`` with ``shm_name`` set and runs
   ``xal_index()``. It then communicates the shared memory names and the
   superblock (via ``xal_get_sb()``) to the other process, for example
   through a Unix socket or another shared memory region.

2. It then calls ``xal_from_shm()`` with the received superblock and shared
   memory region name to obtain a read-only ``struct xal *``::

      const struct xal_sb *sb = /* superblock communicated OOB */;
      const char *shm_name = /* shared memory region name */;
      const char *mountpoint = /* mountpoint of block device, if opened with FIEMAP backend */;
      struct xal *view;

      xal_from_shm(shm_name, sb, mountpoint, &view);
      xal_walk(view, xal_get_root(view), my_callback, NULL);
      xal_close(view); /* frees the struct; does NOT munmap or unlink */

The resulting ``struct xal`` has ``shared_view`` set to ``true``.
``xal_close()`` on a shared view frees only the ``struct xal`` allocation;
the pool memory regions are left mapped and must be unmapped by the caller.
The process that created the shared memory objects is responsible for
``shm_unlink()``.
