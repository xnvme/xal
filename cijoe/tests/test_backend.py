"""
Tests for how xal_open() treats the device it is given.

The default workflow leaves the filesystem unmounted; tests that need it mounted mount it and
unmount it again. xal runs under sudo, which the device node made with mknod needs.
"""

import os
import re
import time
from pathlib import Path

import pytest

EBUSY = 16
EINVAL = 22
ENAMETOOLONG = 36


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    """Run 'cmd', capturing stdout to an artifact file, and return it stripped."""

    out = _artifacts(cijoe) / "backend_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _is_nvme(dev_path) -> bool:
    """Whether dev_path is an NVMe namespace, or a partition of one, which xNVMe identifies by
    ioctl whatever its name."""

    node = Path("/sys/class/block") / Path(os.path.realpath(dev_path)).name
    return (node / "nsid").exists() or (node.resolve().parent / "nsid").exists()


class _Mounted:
    """Mount the configured device for the block, at mountpoint or the configured one."""

    def __init__(self, cijoe, mountpoint=None):
        self.cijoe = cijoe
        self.dev_path = cijoe.getconf("xal.dev_path", None)
        self.mountpoint = mountpoint or cijoe.getconf("xal.mountpoint", None)

    def __enter__(self):
        src = _capture(self.cijoe, f"findmnt -n -o SOURCE --source {self.dev_path}")
        assert not src, f"{self.dev_path} is already mounted"
        self.cijoe.run(f"sudo mkdir -p '{self.mountpoint}'")
        err, _ = self.cijoe.run(f"sudo mount {self.dev_path} '{self.mountpoint}'")
        assert not err, f"could not mount {self.dev_path}"
        return self

    def __exit__(self, *exc):
        err, _ = self.cijoe.run(f"sudo umount '{self.mountpoint}'")
        assert not err, f"could not unmount {self.mountpoint}"


def _dev_numbers(cijoe, dev_path):
    major, minor = _capture(cijoe, f"stat -c '%t %T' {dev_path}").split()
    return int(major, 16), int(minor, 16)


def test_backend_found_by_device_number(cijoe):
    """A mounted block device named by another node than the mount source is found by its device
    number and indexed through the mount. The node keeps the device's name, as xNVMe identifies a
    block device that is not NVMe by it, and is made on devtmpfs, as /tmp may be nodev."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    alias = Path("/dev/xal_dev_alias") / Path(dev_path).name
    major, minor = _dev_numbers(cijoe, dev_path)
    cijoe.run(f"sudo mkdir -p {alias.parent}")
    try:
        cijoe.run(f"sudo mknod {alias} b {major} {minor}")
        with _Mounted(cijoe) as m:
            out = _capture(cijoe, f"sudo xal --meta {alias}")
    finally:
        cijoe.run(f"sudo rm -rf {alias.parent}")

    assert "xal_be_fiemap:" in out, f"the mounted device was not opened with FIEMAP:\n{out}"
    assert f"mountpoint: {m.mountpoint}" in out, out


def test_backend_found_at_escaped_mountpoint(cijoe):
    """A mount point with a space, which the kernel writes as \\040, is found as it is."""

    try:
        with _Mounted(cijoe, "/tmp/xal testing space") as m:
            out = _capture(cijoe, f"sudo xal --meta {m.dev_path}")
    finally:
        cijoe.run("sudo rmdir '/tmp/xal testing space'")

    assert f"mountpoint: {m.mountpoint}" in out, f"the mount point was not found:\n{out}"


def test_backend_reports_long_mountpoint(cijoe):
    """A mount point longer than XAL_PATH_MAXLEN is reported, not taken for no mount, which would
    have the XFS backend parse the mounted filesystem."""

    top = "/tmp/xal_long"
    path = f"{top}/{'a' * 100}/{'b' * 100}/{'c' * 60}"
    try:
        with _Mounted(cijoe, path) as m:
            out = _capture(cijoe, f"sudo xal --meta {m.dev_path}")
    finally:
        cijoe.run(f"sudo rm -rf --one-file-system {top}")

    assert f"xal_open(...); err(-{ENAMETOOLONG})" in out, out


def test_backend_lba_size_unset_without_identify(cijoe):
    """Through a symlink named unlike a block device that is not NVMe, xNVMe cannot identify it,
    so the LBA size is left unset rather than taken as 1 byte."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    if _is_nvme(dev_path):
        pytest.skip("xNVMe identifies an NVMe namespace by ioctl, whatever its name")

    link = _artifacts(cijoe) / "xal_dev_link"
    cijoe.run(f"ln -sfn {dev_path} {link}")
    try:
        with _Mounted(cijoe):
            named = _capture(cijoe, f"sudo xal --meta {dev_path}")
            linked = _capture(cijoe, f"sudo xal --meta {link}")
    finally:
        cijoe.run(f"rm -f {link}")

    assert re.search(r"sb.lba_blksze: [1-9]", named), named
    assert "sb.lba_blksze: 0" in linked, linked


def test_backend_xfs_reports_read_error(cijoe):
    """When the superblock cannot be read, xal_open() returns that error, not errno. Through a
    symlink named unlike the device, xNVMe gets no geometry for a block device that is not NVMe,
    and dev_read() refuses the read with -EINVAL."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    if _is_nvme(dev_path):
        pytest.skip("xNVMe identifies an NVMe namespace by ioctl, whatever its name")

    link = _artifacts(cijoe) / "xal_dev_link"
    cijoe.run(f"ln -sfn {dev_path} {link}")
    try:
        out = _capture(cijoe, f"xal --backend xfs --meta {link}")
    finally:
        cijoe.run(f"rm -f {link}")

    assert f"xal_open(...); err(-{EINVAL})" in out, out


def test_backend_xfs_refuses_mounted_device(cijoe):
    """The XFS backend does not parse the on-disk format of a mounted filesystem."""

    with _Mounted(cijoe) as m:
        out = _capture(cijoe, f"xal --backend xfs --meta {m.dev_path}")

    assert f"xal_open(...); err(-{EBUSY})" in out, f"a mounted device was parsed raw:\n{out}"


def test_backend_xfs_refuses_claimed_device(cijoe):
    """Nor that of a device someone else holds exclusively, as a dm or md device does."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    fd = os.open(dev_path, os.O_RDONLY | os.O_EXCL)
    try:
        out = _capture(cijoe, f"xal --backend xfs --meta {dev_path}")
    finally:
        os.close(fd)

    assert f"xal_open(...); err(-{EBUSY})" in out, f"a claimed device was parsed raw:\n{out}"


def test_backend_xfs_keeps_device_from_mount(cijoe):
    """While the XFS backend reads a device, it cannot be mounted, nor opened by another handle
    with the XFS backend; once closed, it can be mounted."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)
    log = _artifacts(cijoe) / "xal_xfs_hold.log"
    pidfile = _artifacts(cijoe) / "xal_xfs_hold.pid"
    cijoe.run(f"rm -f {log} {pidfile}")
    cijoe.run(f"sh -c 'nohup xal_xfs_hold {dev_path} > {log} 2>&1 & echo $! > {pidfile}'")
    pid = int(pidfile.read_text())
    try:
        for _ in range(100):
            if log.exists() and "READY" in log.read_text():
                break
            time.sleep(0.1)
        assert re.search(r"READY err=0", log.read_text()), log.read_text()

        cijoe.run(f"sudo mkdir -p {mountpoint}")
        out = _capture(cijoe, f"sh -c 'sudo mount {dev_path} {mountpoint} 2>&1; echo rc=$?'")
        if "rc=0" in out:
            cijoe.run(f"sudo umount {mountpoint}")
        assert "rc=0" not in out, "the device was mounted while the XFS backend held it"
        assert "busy" in out, f"mount failed for another reason:\n{out}"

        out = _capture(cijoe, f"xal --backend xfs --meta {dev_path}")
        assert f"xal_open(...); err(-{EBUSY})" in out, f"a second handle opened:\n{out}"
    finally:
        cijoe.run(f"kill {pid}")
        for _ in range(100):
            if cijoe.run(f"kill -0 {pid}")[0]:
                break
            time.sleep(0.1)
        cijoe.run(f"rm -f {pidfile}")

    with _Mounted(cijoe):
        pass
