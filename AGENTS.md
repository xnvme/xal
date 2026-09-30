# AGENTS.md

This file provides guidance to coding agents working with code in this repository. Commit message and trailer rules are in CONTRIBUTING.md.

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

Unit tests in `tests/unit/` need no device. Run them with `make unit-test` after `make build`; CI runs them before installing.

The other tests use [cijoe](https://github.com/refenv/cijoe) (v0.9.51) and require a block device formatted with XFS. The test workflow creates a loopback device, formats it, populates files, then runs pytest tests in `cijoe/tests/`.

The C programs in `tests/integration/` are installed as `xal_test_*` and run by the cijoe tests.

```bash
make test               # alias for test-using-loop
make test-using-loop    # loopback device (default, used in CI)
make test-using-zram    # zram device
make test-using-nbd     # NBD device
make test-using-nvme    # real NVMe device
```

Test configs are in `cijoe/configs/`. The loop config uses `/dev/loop7` and mounts at `/tmp/xal_testing`.

Individual tests live in `cijoe/tests/` (e.g., `test_bmap.py`, `test_index.py`, `test_handle.py`). Tests run via cijoe on the local machine and compare xal output against `xfs_bmap`/`xfs_io` reference output.

## Dependencies

- xNVMe 0.8.0 or later (built from source via `toolbox/pkgs/debian.sh`)
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

1. `xal_open()` -- reads superblock + AG headers (XFS) or `statfs()` (FIEMAP), sets up memory pools
2. `xal_dinodes_retrieve()` -- reads all inodes from disk (XFS backend only)
3. `xal_index()` -- builds the in-memory directory/file tree
4. `xal_walk()` / `xal_get_inode()` / `xal_get_extents()` -- query the tree

### Memory pools (`struct xal_pool`)

Inodes and extents are stored in separate contiguous pools backed by `mmap`. Pools reserve a large virtual range with `PROT_NONE` and commit pages lazily via `mprotect`. Elements are accessed by index (`xal_inode_at(xal, idx)`), never by pointer, so indices stay stable. Pools can optionally use POSIX shared memory (`shm_name` option) for cross-process sharing via `xal_from_shm()`.

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
- `include/xal_be_xfs.h` -- XFS backend internals (allocation group state)
- `include/xal_be_fiemap.h` -- FIEMAP backend internals, shared with the inotify watcher
- `include/xal_be_fiemap_inotify.h` -- inotify watcher state
- `include/xal_be_fiemap_lease.h` -- per-file lease state for `XAL_WATCHMODE_READ_LEASE`
- `include/xal_pool.h` -- memory pool definitions
- `include/xal_pp.h` -- pretty-printer declarations
- `src/xal.c` -- core API implementation
- `src/xal_be_xfs.c` -- XFS backend (direct on-disk parsing)
- `src/xal_be_fiemap.c` -- FIEMAP backend
- `src/xal_be_fiemap_inotify.c` -- inotify watcher for FIEMAP backend
- `src/xal_be_fiemap_lease.c` -- read leases for FIEMAP backend (`XAL_WATCHMODE_READ_LEASE`)
- `src/xal_pool.c` -- memory pool implementation
- `src/pp.c` -- pretty-printers for xal structs
- `src/utils.c` -- internal utility functions
- `src/cli.c` -- `xal` command-line tool
- `tools/xal_bmap_mp_yaml.c` -- multi-process bmap tool (shared-memory pools)
- `tools/xal_snapshot_hold.c` -- test aid holding a reflink snapshot open
- `tools/xal_lease_hold.c` -- test aid holding a file's extents under a read lease
- `tools/xal-server/` -- daemon publishing xal indexes over POSIX shared memory (optional, needs tomlc17)
- `tests/integration/test_inotify.c` -- test of the FIEMAP backend's inotify watcher
- `tests/integration/test_mark_dirty.c` -- test that `xal_mark_dirty()` makes xal-server re-index
- `tests/integration/test_shm.c` -- test of sharing an index between processes with `xal_from_shm()`
- `tests/integration/test_shm_version.c` -- test that `xal_from_shm()` refuses a mismatched state version
- `tests/test.h` -- `TEST_ASSERT()` and `TEST_RUN()` for the C tests
- `tests/unit/test_pool.c` -- unit tests for `xal_pool`
- `tests/unit/test_xal.c` -- unit tests for the core functions in `src/xal.c`
- `tp/khash.h` -- third-party hash map (used for file lookup mode)

## Code style

- C11 (`gnu11`), LLVM-based clang-format with tabs (width 8), 100-column limit
- Format code: `make format` (staged changes) or `make format-all` (all files), via pre-commit
- Function return type on its own line
- Opening brace on new line for functions, same line for control flow (1TBS variant)
- XFS on-disk values are big-endian; all public structs use native byte order
- Error convention: return 0 on success, negative errno on failure
- Variables may be declared at the top of any block, including a `for` body. Only a declaration after statements in the same block breaks this rule
- Headers are not self-contained. Do not add an `#include` to `include/*.h` to satisfy a type a header uses; add the prerequisite to each `.c` file that needs it, above the dependent header. Forward declarations in headers are fine. This is with one exception: `include/libxal.h` is installed as a public header, so this must include necessary headers.
- Include `libxal.h` before `xal.h`, and both before `xal_be_fiemap.h`
- Keep comments to one or two `//` lines stating the reason. Do not write multi-line `/* ... */` blocks explaining races, invariants or rationale; put that in the reply or commit message
- Do not delete existing comments
- Prefer the smallest diff that fixes the problem over a more defensive scheme that needs explaining

## Project scope

- xal only targets XFS, including through the FIEMAP backend. Problems that only matter on btrfs, bcachefs, overlayfs or other file systems are out of scope
- Old Linux kernels are not supported. Problems that only occur on them, such as a missing statx attribute or ioctl, are out of scope

## Code review

- Report only defects the diff introduced. Blame the line before writing a finding, and classify each sub-case separately
- Leave out pre-existing bugs entirely, including under a separate heading
- Leave out notes about code that works as intended
