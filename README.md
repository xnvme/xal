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

#### Inotify watching

When opened with a `watch_mode` other than `XAL_WATCHMODE_NONE`, an
inotify watch is registered for every directory during `xal_index()`.
A background thread started with `xal_watch_filesystem()` then processes
events. The watched event mask per directory is: `IN_CREATE`,
`IN_DELETE`, `IN_MOVE`, `IN_MODIFY`, `IN_ATTRIB`,
`IN_CLOSE_WRITE`, and `IN_UNMOUNT`.

#### Watch modes

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
