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

And optionally,
- `libbpf`, `libelf`, `zlib` (for the BPF event listener)
- `clang`, `llvm`, and `bpftool` (to compile BPF objects and generate skeletons)
- A kernel exposing BTF at `/sys/kernel/btf/vmlinux` (i.e. built with `CONFIG_DEBUG_INFO_BTF=y`)

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

Using inotify and BPF, **xal** monitors changes to the indexed files if a
fitting watchmode is set.

**`XAL_WATCHMODE_NONE`**
: No inotify setup. The xal struct will never be marked dirty automatically.

**`XAL_WATCHMODE_DIRTY_DETECTION`**
: Any filesystem event marks the xal struct as dirty. The caller detects
  this with `xal_is_dirty()` and must re-call `xal_index()` to rebuild
  the tree.

**`XAL_WATCHMODE_EXTENT_UPDATE`**
: File-modification events (`IN_MODIFY`, `IN_CLOSE_WRITE`) trigger an
  automatic in-place extent refresh for the affected file via a new FIEMAP
  call, coordinated with `seq_lock` so concurrent readers remain safe.
  Structural changes (`IN_CREATE`, `IN_DELETE`, `IN_MOVE`) still mark
  the struct dirty, as they require a full re-index.

Either mode ignores events on entries the index skips: symlinks, special
files and mount points.

When opened with `XAL_WATCHMODE_DIRTY_DETECTION` or
`XAL_WATCHMODE_EXTENT_UPDATE`, an inotify watch is registered for every
directory during `xal_index()`.
A background thread started with `xal_watch_filesystem()` then processes
events. The watched event mask per directory is: `IN_CREATE`,
`IN_DELETE`, `IN_MOVE`, `IN_MODIFY`, `IN_ATTRIB`,
`IN_CLOSE_WRITE`, and `IN_UNMOUNT`.

BPF is used to monitor changes made by the filesystem itself. However, this module
is only loaded if the dependencies are present.

#### Read leases

**`XAL_WATCHMODE_READ_LEASE`**
: `xal_index()` builds the tree without extents. `xal_extents_acquire()`
  maps a file the first time it is asked for, while holding a read lease on
  it (`F_SETLEASE`, `F_RDLCK`) and with `FIEMAP_FLAG_SYNC`, so dirty data is
  written back first. Unwritten extents, which read as zeros, are left out,
  and extents past the end of the file are left out or cut at the block
  holding it. A writer that opens or truncates
  the file then waits until every caller between `xal_extents_acquire()`
  and `xal_extents_release()` is done; xal releases the lease, the writer
  goes ahead, and the next `xal_extents_acquire()` maps the file again. A
  file open for writing gets no lease, and `xal_extents_acquire()` returns
  `-EAGAIN` for it.

Where `XAL_WATCHMODE_REFLINK_SNAPSHOT` lets writers go ahead and keeps the
old data for readers, this mode makes writers wait for readers, and writes
nothing to the filesystem itself. It needs:

* the files to be owned by the caller, or `CAP_LEASE`
* one file descriptor per file under a lease. Past `xal_opts.lease_max`, by
  default half the soft `RLIMIT_NOFILE`, or when out of descriptors, xal
  drops the lease of a file without readers that was not acquired since the
  last pass, and maps it again on its next `xal_extents_acquire()`
* readers to release within `/proc/sys/fs/lease-break-time` seconds, after
  which the kernel lets a waiting writer through anyway; a later
  `xal_extents_release()` returns `-ESTALE` once xal's thread has taken the
  break. A reader that holds the extents for long can poll
  `xal_extents_recalled()`, which takes no lock, and release once a writer
  waits. A `lease-break-time` of 0 makes the kernel wait for readers however
  long they take, for every lease on the system
* the caller not to use `SIGRTMIN+7`, nor to rely on `SIGIO` sent to the
  process, while a handle in this mode is open

Breaks reach a thread of xal's as `SIGRTMIN+7`, or `SIGIO`, directed at it
with `F_SETOWN_EX`; it blocks all signals and takes these with
`sigtimedwait()`, so no signal disposition of the process changes. A writer
that opens with `O_NONBLOCK`, as coreutils `truncate` does, does not wait:
it gets `EWOULDBLOCK` while any lease is held on the file, and succeeds on a
retry once xal has released it. The leases belong to the process that opened
the handle; in a forked child `xal_extents_acquire()` and `xal_index()`
return `-ECHILD`.

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

It does not support `shm_name`. A file with delayed allocation, inline,
encoded or encrypted data is refused with `-EOPNOTSUPP`. See
`xal_extents_acquire()` and `xal_extents_release()` in `include/libxal.h`.

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
