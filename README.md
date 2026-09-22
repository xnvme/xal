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

`XAL_WATCHMODE_DIRTY_DETECTION` and `XAL_WATCHMODE_EXTENT_UPDATE` used to sit
between these two. Both were inotify-based attempts at keeping extents usable
under foreign writes, and both reported a change after the fact, which narrows a
race rather than closing it; `XAL_WATCHMODE_REFLINK_SNAPSHOT` pins the blocks
instead. They are gone, and the enum was renumbered rather than left with a hole
in it -- anything outside these two values is `-EINVAL`.

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
