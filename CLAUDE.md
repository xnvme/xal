# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is xal

xal (eXtent Access Library) is a C library for accessing file extent maps from storage devices. It provides a backend-agnostic API with two backends: FIEMAP (uses the kernel's FS_IOC_FIEMAP ioctl on mounted filesystems) and XFS (parses the XFS on-disk format directly, no mount required). It depends on [xNVMe](https://github.com/xnvme/xnvme) for device I/O.

## Build commands

```bash
make configure          # meson setup build --buildtype=release
make build              # meson compile -C build
sudo make install       # meson install -C build
make                    # clean + configure + build + install

# Debug build (enables XAL_DEBUG logging macros via meson option 'debug-logging')
BUILD_TYPE=debug make configure
```

## Testing

Tests use [cijoe](https://github.com/refenv/cijoe) (v0.9.51) and require a block device formatted with XFS. The test workflow creates a loopback device, formats it, populates files, then runs pytest tests in `cijoe/tests/`.

```bash
make test               # alias for test-using-loop
make test-using-loop    # loopback device (default, used in CI)
make test-using-zram    # zram device
make test-using-nbd     # NBD device
make test-using-nvme    # real NVMe device
```

Test configs are in `cijoe/configs/`. The loop config uses `/dev/loop7` and mounts at `/tmp/xal_testing`.

Individual tests live in `cijoe/tests/` (e.g., `test_bmap.py`, `test_index.py`, `test_handle.py`). Tests run remotely via cijoe and compare xal output against `xfs_bmap`/`xfs_io` reference output.

## Dependencies

- xNVMe >= 0.7.0 (built from source via `toolbox/pkgs/debian.sh`)
- meson build system
- `xfslibs-dev`, `xfsprogs` (for XFS headers/tools)
- `librt` (POSIX shared memory / timers)
- cijoe for testing

## Architecture

### Backends

xal has two backends (`enum xal_backend`):

- **XAL_BACKEND_XFS** -- reads the XFS on-disk format directly from the block device (no mount needed). Parses superblock, allocation groups, inode B+trees, directory formats, and file extent B+trees.
- **XAL_BACKEND_FIEMAP** -- uses the Linux FIEMAP ioctl on a mounted filesystem to get extent info. Supports inotify-based filesystem watching (`xal_watch_filesystem()`).

Backend is auto-detected: if the device is mounted, FIEMAP is used; otherwise XFS.

### Core data flow

1. `xal_open()` -- reads superblock + AG headers, sets up memory pools
2. `xal_dinodes_retrieve()` -- reads all inodes from disk (XFS backend only)
3. `xal_index()` -- builds the in-memory directory/file tree
4. `xal_walk()` / `xal_get_inode()` / `xal_get_extents()` -- query the tree

### Memory pools (`struct xal_pool`)

Inodes and extents are stored in separate contiguous pools backed by `mmap`. Pools reserve a large virtual range with `PROT_NONE` and commit pages lazily via `mprotect`. Elements are accessed by index (`xal_inode_at(xal, idx)`), never by pointer, so indices stay stable. Pools can optionally use POSIX shared memory (`shm_name` option) for cross-process sharing via `xal_from_pools()`.

### Key internal types

- `struct xal` (opaque, defined in `include/xal.h`) -- main handle containing device, pools, superblock, backend
- `struct xal_inode` -- normalized inode: name, type, size, children (dentries) or extents
- `struct xal_extent` -- extent record: offset, block, count, flag
- `struct xal_sb` -- superblock subset in native byte order

### Source layout

- `include/libxal.h` -- public API
- `include/libxal_util.h` -- debug macros (`XAL_DEBUG`), static assert, unused-arg helpers
- `include/xal.h` -- internal struct definitions
- `include/xal_odf.h` -- XFS on-disk format constants and structures
- `src/xal.c` -- core API implementation
- `src/xal_be_xfs.c` -- XFS backend (direct on-disk parsing)
- `src/xal_be_fiemap.c` -- FIEMAP backend
- `src/xal_be_fiemap_inotify.c` -- inotify watcher for FIEMAP backend
- `src/xal_pool.c` -- memory pool implementation
- `src/pp.c` -- pretty-printers for xal structs
- `src/utils.c` -- internal utility functions
- `src/cli.c` -- `xal` command-line tool
- `tools/xal_bmap_mp_yaml.c` -- multi-process bmap tool (shared-memory pools)
- `tp/khash.h` -- third-party hash map (used for file lookup mode)

## Code style

- C11 (`gnu11`), LLVM-based clang-format with tabs (width 8), 100-column limit
- Format code: `make format` (staged changes) or `make format-all` (all files), via pre-commit
- Function return type on its own line
- Opening brace on new line for functions, same line for control flow (1TBS variant)
- XFS on-disk values are big-endian; all public structs use native byte order
- Error convention: return 0 on success, negative errno on failure
