# eXtent Access Library

xal is a C library for accessing file extent maps from storage devices with
file systems on them. It
provides a backend-agnostic API -- the same `xal_open()` / `xal_index()` /
`xal_get_extents()` interface works regardless of whether extent information
comes from the kernel (FIEMAP ioctl) or from direct on-disk parsing (XFS
format).

Use cases include:

- Querying file extent maps on mounted filesystems via FIEMAP
- Accessing file data when the storage device driver is detached from the OS:
  - Operating in user space (e.g., SPDK/NVMe, xNVMe/uPCIe)
  - Operating on a peripheral device (e.g., xNVMe/uPCIe/CUDA)

The focus is solely on an efficient data-access library, with no interest in
forensics, recovery, or repair.

## Building

Dependencies:

- `xnvme` >= 0.7.0 -- must be installed and visible to `pkg-config`
- `librt`

The default `make` target runs clean, configure, build, and install in one
shot:

```bash
make
```

For a debug build:

```bash
BUILD_TYPE=debug make
```

Individual targets are also available:

```bash
make configure
make build
make install
make test
```

## Backends

### FIEMAP

The FIEMAP backend uses the kernel's `FS_IOC_FIEMAP` ioctl to read file
extent information, and standard `opendir`/`readdir` to walk the directory
tree. The filesystem **must** be mounted. This backend provides a simpler
integration and supports path-based inode and extent lookup via `xal_get_inode()`/
`xal_get_extents()`.

#### File system changes

**`XAL_WATCHMODE_NONE`**
: Nothing is watched and nothing is pinned. The extents describe the filesystem
  as it was at `xal_index()` time, and a write by any other process can
  invalidate them. For a caller that owns the filesystem; `xal_mark_dirty()` is
  available to signal a change the caller made itself.

**`XAL_WATCHMODE_REFLINK_SNAPSHOT`**
: Every regular file, optionally restricted to `opts.subtree`, is
  reflink-cloned into a private immutable shadow directory at `xal_index()`
  time, and the extents are captured from the clones. The clones hold the
  blocks for as long as the index that produced them stands, so no foreign
  write can move a block out from under a published extent. A re-index
  re-snapshots and releases them; clones are removed at `xal_close()`.

: An inotify watch runs alongside the clones, at whole-index granularity. The
  dirty flag means something different here than for a mode with nothing
  pinned: `xal_is_dirty()` becoming true says *the filesystem has moved on*,
  not *your extents may be garbage*. See `enum xal_watchmode` in `libxal.h`
  for what the pinning does and does not guarantee, and `xal_get_extents()`
  for the sequence-lock loop a reader owes it.

While the index is clean, the watch ignores events on entries the index skips:
symlinks, special files and mount points.

##### What the watch can and cannot see

The watch is one inotify watch per directory, placed during the index walk,
with the mask `IN_CREATE | IN_DELETE | IN_MOVE | IN_MODIFY | IN_ATTRIB |
IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF | IN_UNMOUNT`. One watch per
directory means a large tree needs a correspondingly large
`fs.inotify.max_user_watches` -- it is a per-UID budget shared with every other
inotify user on the system, and a watch that cannot be placed fails the index.

The walk places them whether or not `xal_watch_filesystem()` is ever called, so
a caller that wants only the pinned extents pays the same budget as one that
wants the dirty flag, and can be refused an index over a signal it never reads.
Size `fs.inotify.max_user_watches` for the tree even when nothing will watch it.

Two changes are invisible to it, and no mask fixes either:

- **A writer holding a mapping.** `mmap()` writes produce no event until
  `munmap()` or the last close, so a long-lived mapped writer is unreported for
  as long as it holds the mapping.
- **A hardlink written through a path outside the indexed tree.** inotify
  reports to the watch on the parent directory used for the operation, so a
  write through a second link elsewhere notifies that directory, not ours.

Neither can make an extent invalid -- the clone still pins the blocks -- so the
cost is staleness that goes unreported, not a bad read. Both would be covered
by a filesystem-wide `fanotify` mark, which needs `CAP_SYS_ADMIN`.

#### Read leases

**`XAL_WATCHMODE_READ_LEASE`**
: `xal_index()` builds the tree without extents. `xal_extents_acquire()`
  maps a file the first time it is asked for, while holding a read lease on
  it (`F_SETLEASE`, `F_RDLCK`), as `xal_index()` maps it in
  `XAL_WATCHMODE_NONE`, flags included; extents past the end of the file are
  left out or cut at the block holding it. A writer that opens or truncates
  the file then waits until every caller between `xal_extents_acquire()`
  and `xal_extents_release()` is done; xal releases the lease, the writer
  goes ahead, and the next `xal_extents_acquire()` maps the file again. A
  file open for writing gets no lease, and `xal_extents_acquire()` returns
  `-EAGAIN` for it. `xal_get_extents()` and `xal_walk()` report no extents
  for a file until it has been acquired, nor after its lease is gone.

A caller that opens its files and registers them before reading can hand the
descriptor to `xal_extents_attach_fd()`, which takes the lease on it and maps
the file then, so the first `xal_extents_acquire()` makes no system call
either. The caller keeps the descriptor open until `xal_extents_detach_fd()`,
`xal_index()` or `xal_close()`; xal never closes it. On a descriptor open for
writing the lease is a write lease: it needs the file to be open nowhere else,
and any other `open()` or `truncate()` of the file then waits for the readers.
Writes through that descriptor do not wait; the next `xal_extents_release()`
finds them by the file's size and ctime and returns `-ESTALE`, and the next
acquire maps the file again.

Where `XAL_WATCHMODE_REFLINK_SNAPSHOT` lets writers go ahead and keeps the
old data for readers, this mode makes writers wait for readers, and writes
nothing to the filesystem itself. It needs:

* the files to be owned by the caller, or `CAP_LEASE`, and
  `/proc/sys/fs/leases-enable` to be 1
* one file descriptor per file under a lease, except attached files, which
  use the caller's. Past `xal_opts.lease_max`, by default half the soft
  `RLIMIT_NOFILE`, or when out of descriptors, xal drops the lease of a file
  without readers that was not acquired since the last pass, and maps it
  again on its next `xal_extents_acquire()`
* readers to release within `/proc/sys/fs/lease-break-time` seconds, after
  which the kernel lets a waiting writer through anyway; a later
  `xal_extents_release()` returns `-ESTALE` once xal's thread has taken the
  break
* the caller not to use `SIGRTMIN+7`, nor to rely on `SIGIO` sent to the
  process, while a handle in this mode is open

Breaks reach a thread of xal's as `SIGRTMIN+7`, or `SIGIO`, directed at it
with `F_SETOWN_EX`; it blocks all signals and takes these with
`sigtimedwait()`, so no signal disposition of the process changes. A writer
that opens with `O_NONBLOCK`, as coreutils `truncate` does, does not wait:
it gets `EWOULDBLOCK` while any lease is held on the file, and succeeds on a
retry once xal has released it.

The leases belong to the process that opened the handle; in a forked child
`xal_extents_acquire()` and `xal_index()` return `-ECHILD`. A child made
without the fork handlers, as by `clone()` or `_Fork()`, is not told apart
and must not use or close the handle. `fork()` waits for xal's locks, so do
not fork while holding a lock that a fork handler of the caller takes, nor
from a signal handler while a handle in this mode is open. A file being
mapped at `fork()` stays open in the child, even after `xal_close()` there,
until it exits or execs.

`xal_extents_release()` returns `-ESTALE` when the kernel has let a writer
past the broken lease, so the caller knows to drop what it read. Changes that
do not open the file for writing do not wait for readers: a dedupe through a
read-only descriptor, or data moved by the filesystem itself as zoned XFS
garbage collection does. The release therefore also maps the file again and
returns `-ESTALE` when its extents changed, at the cost of a FIEMAP per
release; on XFS, releases of files that have ever shared blocks, as copies
made by `cp --reflink` have, also wait for each other on the allocation
group. `xal_opts.lease_no_recheck` leaves the FIEMAP out. Writes to the block
device itself are not seen.

With `xal_extents_attach_fd()` the lease is taken on the caller's own
descriptor, a read lease when it is open read-only and a write lease
otherwise. The descriptor must stay open, and name that file, until
`xal_extents_detach_fd()`, `xal_index()` or `xal_close()`: closing it drops
the lease without xal noticing. Its open file must not hold a lease of the
caller's own, and may be attached once: not again through a `dup()`, nor for
another name of the file, nor in another handle. Open it with `O_CLOEXEC`; a
child forked without exec keeps the lease.

A write lease is granted only while the descriptor's open file is the file's
only one. Any other `open()`, a read-only or failed `O_NONBLOCK` one
included, and `truncate()` break it, and while that open file stays open
`xal_extents_acquire()` returns `-EAGAIN`. A thread holding the extents must
therefore not open the file itself: the open waits for its own release, up
to lease-break-time. Writes, `ftruncate()`, `fallocate()`, clones and stores
through a shared mapping of the descriptor break nothing. They change the
file's ctime, and the next `xal_extents_release()` returns `-ESTALE` when the
size or ctime differs from the mapping's; a change of the file's metadata,
as `chmod()`, `setxattr()` or a new link, does too. Not seen: writes through
a descriptor opened with `XFS_IOC_OPEN_BY_HANDLE`, which updates no ctime;
without multigrain timestamps (XFS before Linux 6.13), a write in the same
clock tick as the mapping; and an `O_DIRECT` write still in flight while the
file is mapped, which the release's FIEMAP finds unless
`xal_opts.lease_no_recheck` is set.

It does not support `shm_name`. See `xal_extents_acquire()`,
`xal_extents_release()` and `xal_extents_attach_fd()` in `include/libxal.h`.

#### File lookup modes

The path-based inode and extent lookup implementation depends on which
*lookup mode* is selected.

**`XAL_FILE_LOOKUPMODE_TRAVERSE`**
: Default. Searches the in-memory tree from `xal->root` using binary
  search at each directory level. Entries are sorted alphabetically at
  index time to make this possible.

**`XAL_FILE_LOOKUPMODE_HASHMAP`**
: At index time every inode is inserted into a hash map keyed by its
  absolute path. `xal_get_inode()` then resolves in O(1). Trade-off:
  higher memory usage proportional to the number of inodes.

### XFS

The XFS backend reads directly from the raw block device by parsing the XFS
on-disk format. The filesystem does **not** need to be mounted -- this is the
primary use-case for direct device access, enabling access from user-space
drivers (e.g., SPDK/NVMe, xNVMe/uPCIe) or peripheral devices (e.g.,
xNVMe/uPCIe/CUDA) where the OS has no control over the storage.

There is no notification infrastructure for this backend, since the filesystem
is not mounted and changes are not expected.

For details on the XFS on-disk format as parsed by this backend, see
[docs/xfs-internals.md](docs/xfs-internals.md).

### Auto-detection

If `opts.be` is left as 0, `xal_open()` auto-selects the backend: if the
device URI is found in `/proc/mounts` the FIEMAP backend is chosen,
otherwise XFS is used.

## API Usage

The typical call sequence is:

1. Open a device handle with `xnvme_dev_open()`.
2. Call `xal_open()` to read the superblock and AG headers into `struct xal`.
3. Call `xal_dinodes_retrieve()` to read all inodes from disk.
4. Call `xal_index()` to build the in-memory directory tree rooted at `xal->root`.
5. Use `xal_get_root()`, `xal_walk()`, `xal_get_inode()`, `xal_get_extents()`, etc.
6. Call `xal_close()` and `xnvme_dev_close()` when done.

Example:

```c
struct xnvme_opts xnvme_opts = {0};
struct xal_opts opts = {0};
struct xnvme_dev *dev;
struct xal *xal;
int err;

xnvme_opts_set_defaults(&xnvme_opts);
dev = xnvme_dev_open("/dev/nvme0n1", &xnvme_opts);

err = xal_open(dev, &xal, &opts);
err = xal_dinodes_retrieve(xal);
err = xal_index(xal);

xal_walk(xal, xal_get_root(xal), my_callback, NULL);

xal_close(xal);
xnvme_dev_close(dev);
```

## Limits

Unlike filesystem-specific tools such as `xfs_bmap`, **xal** stores only file
extents, not directory extents. This is intentional, as **xal** provides a data
structure containing the parsed contents of directory extents via
`xal_index()`.

Instead of reading directory blocks from disk, one can use the, in-memory,
decoded file system tree rooted at `xal->root`.

Only directories and regular files are indexed. Symlinks and special files
(FIFOs, sockets, device nodes) are skipped, and the FIEMAP backend skips
anything mounted inside the tree, detected with `STATX_ATTR_MOUNT_ROOT` (Linux
5.8 or later). A mountpoint or `opts.subtree` that is itself a symlink is
rejected with `-ENOTDIR`.
