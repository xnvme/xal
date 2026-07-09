"""
Tests for XAL_WATCHMODE_REFLINK_SNAPSHOT (the FIEMAP reflink-snapshot mode).

These drive the ``xal_snapshot_hold`` helper, which opens the FIEMAP backend in reflink-snapshot
mode, indexes (reflinking regular files into ``<mnt>/.xal_snapshot.<pid>``), announces readiness,
and then holds the snapshot open until signalled -- so a test can inspect the clones, force CoW on
the origins, and confirm the snapshot stays pinned. Requires an XFS mount formatted with reflink=1,
and runs the privileged steps under sudo (immutable needs CAP_LINUX_IMMUTABLE).
"""

import logging as log
import re
import time
from pathlib import Path

import pytest

READY_RE = re.compile(r"SNAPSHOT_READY pid=(\d+) dir=(\S+)")


def _artifacts(cijoe) -> Path:
    # run_local: the artifacts path is a local dir; ensure it exists for capture/log files.
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _require_fixture(cijoe):
    """Resolve (dev_path, mountpoint) and assert the fixture is a reflink XFS actually mounted
    from dev_path -- guards against a stale mount from a prior run shadowing the configured
    device (which would silently test the wrong filesystem)."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, _ = cijoe.run(f"xfs_info {mountpoint} | grep -q 'reflink=1'")
    if err:
        pytest.skip(f"{mountpoint} is not an XFS(reflink=1) mount")

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    assert src == dev_path, (
        f"{mountpoint} is mounted from {src!r}, not the configured dev_path {dev_path!r} "
        f"(stale mount from a previous run?)"
    )
    return dev_path, mountpoint


def _capture(cijoe, cmd) -> str:
    """Run 'cmd', capturing stdout to an artifact file, and return it stripped."""

    out = _artifacts(cijoe) / "reflink_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _first_phys(cijoe, path) -> str:
    """Physical block of extent 0 (filefrag units), or '' if none. Needs sudo (root-owned clones)."""

    return _capture(
        cijoe,
        f"sudo filefrag -v {path} | awk '/^[[:space:]]*0:/{{split($4,a,\".\"); print a[1]; exit}}'",
    )


def _ino(cijoe, path) -> str:
    return _capture(cijoe, f"sudo stat -c %i {path}")


def _start_hold(cijoe, dev_path, mountpoint, subtree=None):
    """Start xal_snapshot_hold in the background; return (pid, shadow_dir) once it is ready."""

    log_path = _artifacts(cijoe) / "hold.log"
    cijoe.run(f"sudo rm -f {log_path}")

    sub = f" {subtree}" if subtree else ""
    # nohup + '&' so the daemon-like helper keeps running after this shell returns.
    cijoe.run(
        f"sudo sh -c 'nohup xal_snapshot_hold {dev_path} {mountpoint}{sub} "
        f"> {log_path} 2>&1 & echo backgrounded'"
    )

    for _ in range(60):  # up to ~30s for open+index
        if log_path.exists():
            m = READY_RE.search(log_path.read_text())
            if m:
                return int(m.group(1)), m.group(2)
        time.sleep(0.5)

    raise AssertionError(f"xal_snapshot_hold never became ready; log:\n{_tail(log_path)}")


def _tail(path: Path) -> str:
    return path.read_text() if path.exists() else "(no log)"


def _stop_hold(cijoe, pid, shadow_dir):
    """Signal the helper and confirm the shadow dir is purged on close."""

    cijoe.run(f"sudo kill {pid}")
    for _ in range(40):  # up to ~20s for close+purge
        err, _ = cijoe.run(f"sudo test -e {shadow_dir}")
        if err:  # test -e failed => gone
            return
        time.sleep(0.5)
    raise AssertionError(f"shadow dir {shadow_dir} survived helper stop")


def test_reflink_whole_tree(cijoe):
    """Snapshot the whole tree; assert fidelity, immutability (flag+write+unlink), CoW stability,
    and purge-on-close."""

    dev_path, mountpoint = _require_fixture(cijoe)

    origin = f"{mountpoint}/xal_reflink_wt"
    cijoe.run(f"sh -c 'echo original-content > {origin}'")
    cijoe.run("sync")
    ino = _ino(cijoe, origin)

    pid, shadow = _start_hold(cijoe, dev_path, mountpoint)
    try:
        clone = _capture(cijoe, f"sudo sh -c 'ls -d {shadow}/{ino}.* 2>/dev/null | head -1'")
        assert clone, f"no clone for inode {ino} under {shadow}"

        # Fidelity: the clone shares the origin's block at snapshot time.
        ob, cb = _first_phys(cijoe, origin), _first_phys(cijoe, clone)
        assert ob and ob == cb, f"clone did not share origin block ({ob} vs {cb})"

        # Immutable: flagged, and the kernel refuses writes and unlink.
        err, _ = cijoe.run(f"sudo lsattr {clone} | awk '{{print $1}}' | grep -q i")
        assert not err, "clone is not immutable"
        err, _ = cijoe.run(f"echo x | sudo tee -a {clone}")
        assert err, "write to immutable clone was not refused"
        cijoe.run(f"sudo rm -f {clone}")
        err, _ = cijoe.run(f"sudo test -e {clone}")
        assert not err, "immutable clone was removed by rm"

        # CoW stability: appending to the origin diverges its blocks; the clone's stay put.
        cijoe.run(f"sh -c 'echo appended-data >> {origin}'")
        cijoe.run("sync")
        ob2, cb2 = _first_phys(cijoe, origin), _first_phys(cijoe, clone)
        assert ob2 and ob2 != ob, f"origin block did not diverge after append ({ob})"
        assert cb2 == cb, f"clone block moved ({cb} -> {cb2}); snapshot not stable"

        # Isolation at the data level: the clone must not carry the origin's post-snapshot append.
        err, _ = cijoe.run(f"sudo grep -q appended-data {clone}")
        assert err, "clone reflects origin's append; snapshot not isolated"
    finally:
        _stop_hold(cijoe, pid, shadow)

    cijoe.run(f"sudo rm -f {origin}")


def test_reflink_subtree_scoping(cijoe):
    """With a subtree, only files under it are cloned; a file outside it is not."""

    dev_path, mountpoint = _require_fixture(cijoe)

    subtree = f"{mountpoint}/xal_reflink_sub"
    inside = f"{subtree}/inside"
    outside = f"{mountpoint}/xal_reflink_outside"
    cijoe.run(f"mkdir -p {subtree}")
    cijoe.run(f"sh -c 'echo inside > {inside}'")
    cijoe.run(f"sh -c 'echo outside > {outside}'")
    cijoe.run("sync")
    ino_in, ino_out = _ino(cijoe, inside), _ino(cijoe, outside)

    pid, shadow = _start_hold(cijoe, dev_path, mountpoint, subtree=subtree)
    try:
        in_clone = _capture(cijoe, f"sudo sh -c 'ls -d {shadow}/{ino_in}.* 2>/dev/null | head -1'")
        out_clone = _capture(cijoe, f"sudo sh -c 'ls -d {shadow}/{ino_out}.* 2>/dev/null | head -1'")
        assert in_clone, f"in-subtree file {inside} (inode {ino_in}) was not cloned"
        assert not out_clone, f"out-of-subtree file {outside} (inode {ino_out}) was cloned"
    finally:
        _stop_hold(cijoe, pid, shadow)

    cijoe.run(f"sudo rm -rf {subtree} {outside}")


def test_reflink_bogus_subtree_fails(cijoe):
    """A subtree that does not exist is rejected at open() -- the helper exits non-zero."""

    dev_path, mountpoint = _require_fixture(cijoe)

    bogus = f"{mountpoint}/xal_reflink_does_not_exist"
    # Runs synchronously: open() fails fast, so the helper never reaches its blocking hold.
    err, _ = cijoe.run(f"sudo xal_snapshot_hold {dev_path} {mountpoint} {bogus}")
    assert err, "helper accepted a non-existent subtree"
