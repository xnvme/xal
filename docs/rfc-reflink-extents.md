# RFC: Reflink-based extent snapshots for GPU-direct raw reads

## TL;DR

We want to hand a **GPU the physical LBAs of a file** so it can read directly from
NVMe with **no CPU/kernel in the read path**. The problem: a mounted filesystem
**mutates under us**, and a stale/invalid map means silently reading garbage. We
evaluated inotify, BPF tracing, `fsfreeze`, and dm-thin — all either **can't fence
reads**, **block all writers**, or **add a mapping layer** that breaks raw device
access.

**Proposal: at index time, reflink (`FICLONE`) every regular file into a private,
same-device snapshot and read the extents off the clones.** Exposed as a watch mode,
`XAL_WATCHMODE_REFLINK_SNAPSHOT` (optionally scoped to `opts.subtree`). Each
clone shares the origin's blocks but is **hardened immutable**, so writes to the origin
divert to new blocks (CoW) and nothing can relocate the clone — the extents stay valid
for the whole xal session. Clones live on the **same namespace**, so their extents are
**true device LBAs**, and the CPU is involved only in the **one-time index**, not the
read path. `xal_get_extents()` is unchanged. The approach effectively **requires XFS**.

## Context

xal's end goal is to serve a consumer the **physical LBA ranges** of a file's data so
the data can be read **directly from the NVMe device with no CPU/kernel in the read
path** (GPUDirect-style: the GPU issues block I/O itself). Because there is **no page
cache and no kernel coherence** in that read path, a stale or invalid extent map means
the consumer DMAs the wrong LBAs and **silently reads garbage or another file's data**.
This is a **data-integrity requirement**, not a cache-freshness nicety.

The hard part is that a mounted filesystem **mutates behind us**. We need extents that
are **(a) authoritative** for on-disk state and **(b) stable** for the duration of the
read.

## Approaches considered and rejected

- **inotify → re-FIEMAP.** VFS/namespace notification, *not* block allocation.
  Coalesces, drops events on queue overflow, and fires at `write()`/`close` — with XFS
  **delayed allocation** the extent may still be an unresolved delalloc reservation at
  that point. *Lossy and racy* against the thing we care about.
- **BPF tracing of bmap/allocation.** Better timing/granularity, but observes the
  *in-core* extent mutation inside an **uncommitted transaction** (may abort; log not
  yet committed). It is **observational and after-the-fact**: one missed/reordered event
  desyncs us permanently with no reconciliation anchor. For raw-LBA reads, silent desync
  = corruption. **BPF can observe, it cannot fence.**
- **fsfreeze (freeze-and-hold).** Correct — quiesces writes, flushes delalloc, commits
  the log — but freeze is **whole-filesystem** and blocks *all* writers for the entire
  serving session. **Too broad.**
- **dm-thin snapshot.** CoW snapshot at the block layer; live FS unaffected. But the
  snapshot lives in a **separate pool address space**, so its "physical block" is *not* a
  true device LBA — it requires a **second translation** through the thin metadata btree,
  and reading through `/dev/mapper` puts the **kernel/dm back in the data path**.
  Conflicts with the zero-CPU raw-read goal.

## Key realization

A coherent cache of a *live mutating* FS, with reads that bypass the kernel and zero
writer impact, is **not achievable** with what the kernel exports — it would require a
**pre-invalidation fence** ("block X is about to be reallocated, hold the reader"), and
inotify/fanotify-notify/BPF are all after-the-fact. So instead of caching a mutating
object, **pin an immutable copy**: copy-on-write cloning turns an *impossible coherence
problem* into a *tractable per-file versioning problem*.

## Proposed approach: index-time reflink snapshot

Exposed as a new watch mode, **`XAL_WATCHMODE_REFLINK_SNAPSHOT`** (with optional
**`opts.subtree`** (a general FIEMAP-backend index scope) to restrict which files are captured — the index walk is rerooted
at the subtree, so files outside it are neither traversed nor FIEMAP'd). It hooks the existing
FIEMAP index walk (`xal_be_fiemap_process_inode_file`); there is **no new query API** —
`xal_get_extents()` returns the same pooled extents, which now point at pinned clone
blocks.

**Flow, per regular file, during `xal_index()`:**

1. `FICLONE` the origin into a private shadow directory `<mnt>/.xal_snapshot.<pid>` → a
   clone inode **sharing the origin's physical extents**. `FICLONE` flushes the origin's
   dirty pages and resolves delalloc to real blocks first (see *Data flush* below), so
   raw reads see committed blocks — **no explicit `fsync`**.
2. Set the clone **immutable** (`FS_IMMUTABLE_FL`) so it cannot be relocated (see
   *Extent relocation* below).
3. FIEMAP the clone → store extents in the normal pool (unchanged indexing path).
4. Close the clone fd. The clone is **left on-disk**; its inode keeps the shared blocks
   pinned for the session (option B — see *Lifetime* below).

All clones are removed at `xal_close()`.

**Why this fits:**

- **Same-FS, same device = one address space, one mapping layer.** The clone shares the
  *exact* physical LBAs on the same NVMe namespace; `file offset → fsbno → LBA` is
  **directly issuable** as an NVMe command. No dm layer, no second translation.
- **CPU/kernel only in the one-time index, never in the data path.** Clone + parse is
  control-plane; the GPU then streams raw LBAs.
- **Dissolves the coherence problem.** An immutable clone is a stable snapshot: a write
  to the origin breaks sharing and diverts to new blocks, leaving the clone's blocks
  intact. No freeze, no thaw tripwire, no epoch fencing needed for extent validity.

## Correctness invariant

> **The clone's physical extents must not move or be freed for the read window.**

Everything below protects this invariant; each violation is a **silent-corruption
path**.

### Lifetime / keep-alive — *decided: on-disk shadow dir (option B)*

Clones live as **named files in a shadow directory** `<mnt>/.xal_snapshot.<pid>`, not as
held fds. Their on-disk inodes keep the shared blocks pinned for the session, so we don't
consume an fd per file — this scales to whole-tree snapshots (holding one fd per clone
would hit `RLIMIT_NOFILE`). The clone fd is closed right after FIEMAP; the file stays.
`xal_close()` clears each clone's immutable flag, unlinks it, and removes the directory.

`reflink_sweep_orphans()` purges **every** `<mnt>/.xal_snapshot.*`. It runs at `xal_open()`
(so orphans are cleaned even if the caller never indexes) and again at the **start of each
`xal_index()`**, where it also resets the lazy-create flag so the fresh snapshot is built
from scratch. That one mechanism covers two cases: a crash leaves the shadow dir behind
(immutable clones survive `kill -9`) and it's swept on the next open/index → orphans
self-heal; and a re-index on the same handle drops the previous run's clones → it
re-snapshots the current tree rather than reusing stale clones. Because the index sweep runs
before the walk enumerates the mountpoint, the walk never sees a shadow dir to descend into
(and it also skips `.xal_snapshot.*` defensively). This assumes a single xal instance per
mount; a concurrent instance's live snapshot on the same mount would be swept too.

### Extent relocation of a non-written file — *decided: harden clones immutable*

The invariant assumes *only a write to the clone can move its blocks*. We verified this
against the XFS source (kernel 6.18):

- **Nothing spontaneous relocates committed data extents.** Background reclaim
  (`xfs_blockgc` / `XFS_IOC_FREE_EOFBLOCKS`) touches only post-EOF speculative prealloc and
  CoW-fork staging blocks (`fs/xfs/xfs_icache.c:38`); delalloc writeback only allocates for
  a file being written.
- **Defrag is explicit and userspace-triggered.** `xfs_fsr` (xfsprogs) drives
  `XFS_IOC_SWAPEXT` / `XFS_IOC_EXCHANGE_RANGE`; `xfs_swap_extents()` has a single in-kernel
  caller, the ioctl at `xfs_ioctl.c:972`. No timer/kthread.
- **Defragging the *origin* is safe for us.** swap/exchange *handle* (don't reject) shared
  files, so the origin moves to new blocks while the clone's shared blocks stay alive via
  the refcount btree. Cached extents only go stale if a relocation runs **on the clone
  itself**.

**Decision:** set each clone **immutable** (`FS_IMMUTABLE_FL`) right after `FICLONE`. The
kernel then refuses to relocate it: exchange-range rejects immutable inodes outright
(`fs/xfs/xfs_exchrange.c:352`) and legacy swapext/fsr can't even write-open it
(`fs/namei.c:581`). This removes the need for the previously-planned BPF kprobe tripwire —
the block is enforced at the source, not detected after the fact. Cleanup clears the flag
before `unlink` (`may_delete` refuses immutable inodes, `fs/namei.c:3281`). Requires
`CAP_LINUX_IMMUTABLE` (xal is already privileged for `FIFREEZE`).

Residual, out of scope: neither `xfs_growfs` shrink nor `xfs_scrub` online repair actually
relocates committed data — shrink *refuses* over allocated space rather than evacuating it
(`xfs_ag_shrink_space()` claims the tail via exact-bno allocation and fails `-ENOSPC` if it is
in use, `fs/xfs/libxfs/xfs_ag.c:739`), and online repair rebuilds metadata to describe blocks
in place. The genuine gap immutable cannot reach is **block-layer aliasing** — an LVM snapshot
origin or dm-thin device presenting a different address space than the LBAs FIEMAP reports —
plus repair of an already-corrupt filesystem.

### Data flush — *decided: rely on FICLONE*

No explicit `fsync`. `FICLONE` itself flushes the source: `xfs_file_remap_range` →
`xfs_reflink_remap_prep` → `__generic_remap_file_range_prep` does
`filemap_write_and_wait_range` on the source **under the iolock** (`fs/remap_range.c:324`),
writing back dirty pages and resolving delalloc to real blocks before sharing — exactly
what the clone's FIEMAP needs, serialized against concurrent writers.

A per-file `fsync` (or a global `sync` before the index) would add only device-flush
**durability**, which the live read-while-mounted model doesn't need and which costs N
log-forces on a large tree. A single pre-index `sync` would also be *weaker*: files
dirtied *during* the walk wouldn't be covered, whereas `FICLONE` flushes each file at its
own clone time. (If snapshots ever needed to survive a crash, the `fsync` would return.)

## Namespace staleness (rename / delete / create)

The clones guarantee *extent* validity, not *namespace* freshness. Reflink mode runs with
no inotify watch, so the in-memory tree reflects names as of `xal_index()`; structural
changes after indexing are not tracked until a full re-index. Crucially, none of them can
produce wrong or garbage blocks — only a stale name view:

- **Rename `a` → `b`.** A pure directory-entry change; it does not touch the inode, its
  data, or its blocks, and the clone pins the blocks regardless of the origin's name. So
  extent serving is unaffected. Querying the old path still returns the correct pinned
  extents; querying the new path returns `-ENOENT` (or, if it already existed at index
  time, that entry's own snapshotted extents).
- **Delete.** The clone's refcount keeps the shared blocks alive, so the snapshotted
  extents for that path stay valid and correctly served even after the origin is gone.
- **Create.** New files are simply absent from the snapshot until re-index.

Because clones are named `<ino>.<gen>`, `find <mnt> -inum <ino>` still resolves to the
origin's *current* path after a rename; the generation number also guards against an inode
number being recycled to a different file mid-walk (that file gets its own clone rather than
reusing the earlier one). Refreshing the namespace requires re-running `xal_index()`.

## Filesystem constraint

For the **raw GPU-direct path**, the FS must provide a *direct* extent→device-LBA mapping
with **no internal logical indirection**, plus CoW clone:

- **XFS** — reflink + **flat `fsbno → LBA`** on a single device. **Best fit; effectively
  the target.**
- **Btrfs** — has reflink, but its **chunk-tree logical address space** re-adds an
  internal mapping layer (FIEMAP "physical" is *logical*, not a device LBA). Reintroduces
  the dm-thin wrinkle. Not viable for raw reads except single-device with chunk-tree
  parsing.
- **ZFS** — DVA + RAIDZ indirection; **not viable** for raw LBA reads.
- **ext4** — **no reflink.** Out.

Other filesystems remain usable on the **kernel-FIEMAP-through-mount** path, but the raw
path **pins us to XFS**. Supporting others is not just porting — it **changes the
correctness model**.

## Out of scope

- **ENOSPC** from pinned-clone CoW divergence. The snapshot holds a clone of every
  captured file for the whole session, so sustained writes to the origins force CoW
  allocation that can't be reclaimed until `xal_close()` — a heavy writer can fill the FS
  and fail *writers*. Acknowledged, not addressed here.
- The **unmounted / raw-device XFS backend** (no writers assumed) remains a PoC path.

## Open questions

- Confirm **`xfs_fsr` behavior** on reflinked/shared inodes (does it skip them?) — an
  xfsprogs policy question; immutable already blocks it regardless, so this is
  informational.
