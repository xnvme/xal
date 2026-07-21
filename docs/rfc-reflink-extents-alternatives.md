# RFC addendum: Reflink extent snapshots — alternatives considered

Companion to [`rfc-reflink-extents.md`](rfc-reflink-extents.md). That RFC's "Approaches
considered and rejected" covers alternative *snapshot mechanisms* (inotify, BPF, `fsfreeze`,
dm-thin). This addendum records the alternatives to the **enforcement strategy** — how, and
whether, to keep a captured extent from going stale — because the reflink pin is the costliest
part of the design and it is fair to ask whether it is warranted. Verified against the XFS
source (kernel 6.18).

## Rely on the operational invariant only (no guard)

On XFS a committed, non-shared data extent is **allocated in place and never spontaneously
relocated** — no log-structured cleaner, no `balance`/GC (see the RFC's *Extent relocation*);
even `xfs_growfs` shrink *refuses* rather than evacuating live data (`xfs_ag_shrink_space()`
claims the tail via exact-bno allocation and fails `-ENOSPC` if it is in use,
`fs/xfs/libxfs/xfs_ag.c:739`). In the target workload the consumer is **read-only in the serving
phase**, so no application write can free or CoW a captured block. If defrag/dedupe are also
forbidden by policy, the invariant holds with no machinery at all.

**Rejected as the sole mechanism** because the failure it leaves open — a block freed-then-reused
or relocated out from under a raw LBA — is **silent, undetectable device-read corruption**: the
GPU has no way to know it DMA'd the wrong bytes, there is no checksum or `EIO` on that path. That
puts data integrity entirely on operator discipline. We keep the invariant as the **documented
contract** but back it with the reflink pin, so an operator error (a stray `xfs_fsr`) degrades to
a *versioning* problem rather than corruption.

## Detect staleness instead of preventing it

Cache extents at index time, then **re-validate** before trusting them and re-index on change.
Cheaper than cloning — but every userspace-visible signal fails against the one **silent, no-write
mover**, background defrag (`xfs_fsr` driving `XFS_IOC_EXCHANGE_RANGE`):

- **`ctime`/`mtime` (dirty-detection).** `xfs_fsr` opens targets with `XFS_IOC_OPEN_BY_HANDLE`,
  which sets `FMODE_NOCMTIME` for regular files (`fs/xfs/xfs_handle.c:299`). Exchange-range then
  **skips** the timestamp bump — it gates `__XFS_EXCHANGE_RANGE_UPD_CMTIME*` on `!FMODE_NOCMTIME`
  (`fs/xfs/xfs_exchrange.c:775`). Defrag is deliberately timestamp-invisible, so a `ctime` check
  never sees it.
- **statx change cookie (`i_version`).** This *is* bumped on the exchange commit — the core-log
  path forces it (`xfs_trans_log_inode` → `inode_maybe_inc_iversion(inode, XFS_ILOG_CORE)`), and
  XFS enables `SB_I_VERSION` (`fs/xfs/xfs_super.c:1903`). But `STATX_CHANGE_COOKIE` is
  **kernel-only**: the VFS strips it from the userspace `statx` result (`fs/stat.c:714`).
  Unreachable from the daemon.
- **Re-FIEMAP and diff physical extents.** Correct in principle, but costs a **full re-index per
  file** and is **TOCTOU-racy** — a relocation can land between the re-check and the GPU's raw
  read. A detection pass can observe a past state; it cannot *fence* the read window.

**Conclusion:** no cheap, userspace-visible signal catches the silent mover; only a **positive
pin** (refcount + immutable) both prevents relocation and needs no polling. This is the same
"observe cannot fence" conclusion the RFC reached for BPF, now confirmed for timestamp/version
detection.

## Mark the origins immutable (no clone)

Set `FS_IMMUTABLE_FL` on the **origin** files for the session instead of cloning, saving the
shadow inodes and the CoW-divergence space. **Rejected:** it mutates **real user files'**
persistent state (an externally visible `chattr +i`), **blocks legitimate writers** for the whole
serving session, and a crash leaves the originals immutable with no owning handle to clean them up
— the orphan problem, now on files that matter. It also yields a **frozen origin, not a
point-in-time snapshot**: index-phase appends would be refused, and there is no CoW path to let
writers diverge. Reflink-into-shadow confines all mutation-blocking to **throwaway clones** and
leaves the origins fully writable (writes just divert via CoW).
