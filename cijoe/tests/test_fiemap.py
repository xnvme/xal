"""
Tests for the extents the FIEMAP backend reports for files in particular states.

They run ``xal --backend fiemap --bmap`` and look at the fifth element of each extent, its flag: the
bits of ``enum xal_extent_flag``. Most need the filesystem mounted from ``xal.dev_path``, as the
workflows that keep it mounted leave it, and skip otherwise; the writeback-failure test builds a
filesystem of its own.
"""

import time
from pathlib import Path

import pytest
import yaml

UNWRITTEN = 1 << 0
SHARED = 1 << 1
NOADDR = 1 << 2

FLAKEY = "xal_flakey"


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    out = _artifacts(cijoe) / "fiemap_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _require_mount(cijoe):
    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    if src != dev_path:
        pytest.skip(f"{mountpoint} is not mounted from {dev_path}")
    return dev_path, mountpoint


def _bmap(cijoe, dev_path, mountpoint, rel, sudo=False):
    """The extents of mountpoint/rel as 'xal --backend fiemap --bmap' prints them"""

    out = _artifacts(cijoe) / "fiemap_bmap.yaml"
    prefix = "sudo " if sudo else ""
    err, _ = cijoe.run(f"{prefix}xal --backend fiemap --bmap {dev_path} > {out}")
    assert not err, "xal --backend fiemap --bmap failed"

    bmap = yaml.safe_load(out.read_text()) or {}
    key = f"{dev_path}/{rel}"
    assert key in bmap, f"{mountpoint}/{rel} missing from the index"
    return bmap[key] or []


def test_extent_flags(cijoe):
    """A written file has no flag, a fallocated one is unwritten, and a file and its reflink copy
    are both shared."""

    dev_path, mountpoint = _require_mount(cijoe)
    d = f"{mountpoint}/xal_flags"

    err, _ = cijoe.run(f"rm -rf {d} && mkdir -p {d}")
    assert not err
    err, _ = cijoe.run(f"xfs_io -f -d -c 'pwrite -q 0 1m' {d}/written")
    assert not err
    err, _ = cijoe.run(f"xfs_io -f -c 'falloc 0 1m' {d}/unwritten")
    assert not err

    err, _ = cijoe.run(f"xfs_info {mountpoint} | grep -q 'reflink=1'")
    reflink = not err
    if reflink:
        err, _ = cijoe.run(f"xfs_io -f -d -c 'pwrite -q 0 1m' {d}/origin")
        assert not err
        err, _ = cijoe.run(f"cp --reflink=always {d}/origin {d}/clone")
        assert not err
    cijoe.run("sync")

    try:
        for name, want in [("written", 0), ("unwritten", UNWRITTEN)]:
            extents = _bmap(cijoe, dev_path, mountpoint, f"xal_flags/{name}")
            assert extents, f"{name}: no extents"
            assert {e[4] for e in extents} == {want}, f"{name}: {extents}"

        if reflink:
            for name in ["origin", "clone"]:
                extents = _bmap(cijoe, dev_path, mountpoint, f"xal_flags/{name}")
                assert extents, f"{name}: no extents"
                assert {e[4] for e in extents} == {SHARED}, f"{name}: {extents}"
    finally:
        cijoe.run(f"rm -rf {d}")


def test_unflushed_data_maps_to_its_blocks(cijoe):
    """Data still in the page cache is written back before the file is mapped, so its extents
    carry the blocks it got rather than block 0 and no XAL_EXTENT_NOADDR."""

    dev_path, mountpoint = _require_mount(cijoe)
    f = f"{mountpoint}/xal_unflushed"

    err, _ = cijoe.run("command -v filefrag")
    if err:
        pytest.skip("filefrag is not installed")

    cijoe.run(f"rm -f {f}")
    # Buffered and not synced: well inside the writeback interval, the data is a delayed
    # allocation when xal maps the file.
    err, _ = cijoe.run(f"xfs_io -f -c 'pwrite -q 0 1m' {f}")
    assert not err

    try:
        # Without -s, filefrag maps the file as it is; once written back, there is nothing to test
        err, _ = cijoe.run(f"filefrag -v {f} | grep -q delalloc")
        if err:
            pytest.skip("the data was written back before the test could map it")

        extents = _bmap(cijoe, dev_path, mountpoint, "xal_unflushed")
        assert extents, "no extents"
        for e in extents:
            assert e[2] != 0, f"extent at block 0: {extents}"
            assert not e[4] & NOADDR, f"extent without an address: {extents}"
    finally:
        cijoe.run(f"rm -f {f}")


def _flakey_table(cijoe, loop, sectors, target):
    """Swap the table of the FLAKEY device for one with the given target"""

    for cmd in [
        f"sudo dmsetup suspend --nolockfs {FLAKEY}",
        f"sudo dmsetup load {FLAKEY} --table '0 {sectors} {target}'",
        f"sudo dmsetup resume {FLAKEY}",
    ]:
        err, _ = cijoe.run(cmd)
        assert not err, f"failed: {cmd}"


def test_failed_writeback_does_not_fail_the_index(cijoe):
    """A file whose writeback failed is mapped instead of failing the index: the range whose write
    failed maps as unwritten, and fsync() still reports the failure.

    Writes fail through dm-flakey for about a second, on an XFS of its own over a loop device on
    /dev/shm. A log write in that second would shut the filesystem down; XFS first runs its log
    worker fs.xfs.xfssyncd_centisecs after the mount, and the test checks it was done before."""

    for tool in ["dmsetup", "losetup", "blockdev", "mkfs.xfs", "xfs_io"]:
        err, _ = cijoe.run(f"command -v {tool}")
        if err:
            pytest.skip(f"{tool} is not installed")
    err, _ = cijoe.run("sudo modprobe dm_flakey")
    if err:
        pytest.skip("dm-flakey is not available")
    if _capture(cijoe, "findmnt -n -o FSTYPE -T /dev/shm") != "tmpfs":
        pytest.skip("/dev/shm is not tmpfs")
    cijoe.run("sudo modprobe xfs")
    syncd = _capture(cijoe, "sysctl -n fs.xfs.xfssyncd_centisecs")
    if not syncd.isdigit() or int(syncd) < 1000:
        pytest.skip(f"fs.xfs.xfssyncd_centisecs is {syncd!r}; the test needs 10 s or more")

    img = "/dev/shm/xal_flakey.img"
    mnt = "/dev/shm/xal_flakey.mnt"
    dev = f"/dev/mapper/{FLAKEY}"
    f = f"{mnt}/f"
    loop = ""
    sectors = ""
    created = False

    # Left over from an aborted run; removing its image would strand its loop device
    err, _ = cijoe.run(f"sudo dmsetup info {FLAKEY} || test -e {img}")
    assert err, f"{FLAKEY} or {img} is left over from an earlier run; remove them first"

    try:
        err, _ = cijoe.run(f"truncate -s 512M {img}")
        assert not err
        loop = _capture(cijoe, f"sudo losetup -f --show {img}")
        assert loop.startswith("/dev/loop"), f"losetup gave {loop!r}"
        sectors = _capture(cijoe, f"sudo blockdev --getsz {loop}")
        err, _ = cijoe.run(f"sudo dmsetup create {FLAKEY} --table '0 {sectors} linear {loop} 0'")
        assert not err, f"could not create {FLAKEY}; left over from an earlier run?"
        created = True
        for cmd in [f"sudo mkfs.xfs -q -f {dev}", f"mkdir -p {mnt}"]:
            err, _ = cijoe.run(cmd)
            assert not err, f"failed: {cmd}"
        mounted = time.monotonic()
        err, _ = cijoe.run(f"sudo mount {dev} {mnt}")
        assert not err, "failed to mount"

        err, _ = cijoe.run(f"sudo xfs_io -f -c 'pwrite -q 0 1m' {f}")
        assert not err
        _flakey_table(cijoe, loop, sectors, f"flakey {loop} 0 0 180 1 error_writes")
        cijoe.run(f"sudo xfs_io -c 'sync_range -w 0 0' {f}")
        time.sleep(1)
        _flakey_table(cijoe, loop, sectors, f"linear {loop} 0")
        window = time.monotonic() - mounted
        assert window * 100 < int(syncd), (
            f"writes failed until {window:.1f} s after the mount, past the first log worker run"
        )

        extents = _bmap(cijoe, dev, mnt, "f", sudo=True)
        assert extents, "no extents"
        assert {e[4] for e in extents} == {UNWRITTEN}, f"{extents}"

        out = _capture(cijoe, f"sudo sh -c 'xfs_io -c fsync {f} 2>&1'")
        assert "Input/output error" in out, f"fsync() did not report the failure: {out!r}"
    finally:
        # Linear and resumed before the unmount, as an unmount of a suspended device would hang;
        # suspended without a freeze first, as a freeze would write the log through error_writes
        if created:
            cijoe.run(f"sudo dmsetup suspend --nolockfs {FLAKEY}")
            cijoe.run(f"sudo dmsetup load {FLAKEY} --table '0 {sectors} linear {loop} 0'")
            cijoe.run(f"sudo dmsetup resume --nolockfs {FLAKEY}")
            cijoe.run(f"mountpoint -q {mnt} && sudo umount {mnt}")
            cijoe.run(f"sudo dmsetup remove {FLAKEY}")
        if loop.startswith("/dev/loop"):
            cijoe.run(f"sudo losetup -d {loop}")
        cijoe.run(f"rm -f {img}; rmdir {mnt}")
