"""
Tests for saving an index of the XFS backend and loading it back ('xal --index-file',
xal_index_save(), xal_index_load())

'xal --index-file' loads the index from the file, or indexes the device and saves the index there
when the load fails, and reports on stderr what each call returned. Whichever way it came by the
index, what it prints must be what 'xal' prints without a file.

The tests mount, change and break a filesystem of their own on a loop device, made for each by
the 'fs' fixture, rather than the workflow's, which the other tests share.
"""

import re
from pathlib import Path

import pytest

ENOENT, EPERM, ENODATA, ELOOP, EPROTO, ESTALE, EUCLEAN = 2, 1, 61, 40, 71, 116, 117

IMG = "/tmp/xal_saved.img"
MNT = "/tmp/xal_saved_mnt"


def _run(cijoe, cmd):
    err, state = cijoe.run(cmd)
    assert not err, cmd
    return state.output()


def _make(cijoe, img, populate, mkfs_opts=""):
    """A loop device with a new XFS on 'img', which the user may read; mounted once if 'populate'"""

    _run(cijoe, f"rm -f {img} && truncate -s 512M {img}")
    dev = _run(cijoe, f"sudo losetup -f --show {img}").strip().splitlines()[-1]
    _run(cijoe, f"sudo chown $USER {dev} && sudo mkfs.xfs -f -q {mkfs_opts} {dev}")
    if populate:
        _change(cijoe, dev, "sh -c 'mkdir d && for i in $(seq 0 99); do echo $i > d/f$i; done'")
    return dev


def _change(cijoe, dev, cmd):
    """Mount 'dev', run 'cmd' in it as root, and unmount it, whatever 'cmd' does"""

    _run(cijoe, f"sudo mkdir -p {MNT} && sudo mount {dev} {MNT}")
    try:
        _run(cijoe, f"cd {MNT} && sudo {cmd}")
    finally:
        _run(cijoe, f"sudo umount {MNT}")


@pytest.fixture
def fs(cijoe):
    dev = _make(cijoe, IMG, True)
    yield dev
    cijoe.run(f"sudo umount {MNT}")
    cijoe.run(f"sudo losetup -d {dev}; rm -f {IMG}")


def _xal(cijoe, dev, name, opts=""):
    """Run 'xal --find' on 'dev'; return its output and what it said of the index file"""

    artifacts = Path(cijoe.getconf("xal.artifacts.path"))
    out, log = artifacts / f"saved-{name}.output", artifacts / f"saved-{name}.log"

    _run(cijoe, f"xal --find {opts} {dev} > {out} 2> {log}")
    return out.read_text().replace(dev, ""), log.read_text()


def _index(cijoe, name):
    path = Path(cijoe.getconf("xal.artifacts.path")) / f"{name}.index"
    cijoe.run(f"rm -f {path}")
    return path


def _calls(log, path, load, save=None):
    """Whether 'xal --index-file' said the load returned -'load', and the save -'save' or that
    there was none"""

    if f"xal_index_load({path}); err({-load})\n" not in log:
        return False
    if save is None:
        return "xal_index_save" not in log
    return f"xal_index_save({path}); err({-save})\n" in log


def test_saved_round_trip(cijoe, fs):
    """An index saved and loaded back is the one the device gives."""

    path = _index(cijoe, "round-trip")
    expected, _ = _xal(cijoe, fs, "reference")

    got, log = _xal(cijoe, fs, "first", f"--index-file {path}")
    assert _calls(log, path, ENOENT, 0)
    assert got == expected

    got, log = _xal(cijoe, fs, "second", f"--index-file {path}")
    assert _calls(log, path, 0)
    assert got == expected


def test_saved_stale_after_mount(cijoe, fs):
    """A change made through a mount makes the saved index stale, and the next one is saved."""

    path = _index(cijoe, "mount")
    _xal(cijoe, fs, "mount-save", f"--index-file {path}")

    _change(cijoe, fs, "touch d/added")
    got, log = _xal(cijoe, fs, "mount-after", f"--index-file {path}")
    assert _calls(log, path, ESTALE, 0)
    assert "/d/added\n" in got

    again, log = _xal(cijoe, fs, "mount-again", f"--index-file {path}")
    assert _calls(log, path, 0)
    assert again == got


def test_saved_dirty_log(cijoe, fs):
    """A log a shutdown left dirty is refused, and so is saving the index read from it."""

    path = _index(cijoe, "dirty")
    _xal(cijoe, fs, "dirty-save", f"--index-file {path}")

    _change(cijoe, fs, f"sh -c 'touch d/before-shutdown && xfs_io -x -c \"shutdown -f\" {MNT}'")
    _, log = _xal(cijoe, fs, "dirty-after", f"--index-file {path}")
    assert _calls(log, path, EUCLEAN, EUCLEAN)

    # Mounting replays the log, and its unmount leaves it clean again
    _change(cijoe, fs, "true")
    _, log = _xal(cijoe, fs, "dirty-replayed", f"--index-file {path}")
    assert _calls(log, path, ESTALE, 0)
    _, log = _xal(cijoe, fs, "dirty-again", f"--index-file {path}")
    assert _calls(log, path, 0)


def test_saved_read_only_mounts(cijoe, fs):
    """A read-only mount writes when it is unmounted too, unless it skips the log (norecovery)."""

    path = _index(cijoe, "read-only")
    _xal(cijoe, fs, "read-only-save", f"--index-file {path}")

    _run(cijoe, f"sudo mkdir -p {MNT} && sudo mount -o ro,norecovery {fs} {MNT}")
    _run(cijoe, f"sudo umount {MNT}")
    _, log = _xal(cijoe, fs, "read-only-norecovery", f"--index-file {path}")
    assert _calls(log, path, 0)

    _run(cijoe, f"sudo mount -o ro {fs} {MNT}")
    _run(cijoe, f"sudo umount {MNT}")
    _, log = _xal(cijoe, fs, "read-only-ro", f"--index-file {path}")
    assert _calls(log, path, ESTALE, 0)


def test_saved_changed_before_save(cijoe, fs):
    """A change between reading the inodes and saving the index is refused (test_index_save.c)."""

    path = _index(cijoe, "before-save")
    _run(cijoe, f"xal_test_index_save {fs} {path}")

    change = f"sudo mount {fs} {MNT} && sudo touch {MNT}/d/between && sudo umount {MNT}"
    out = _run(cijoe, f"xal_test_index_save {fs} {path} '{change}'")
    assert f"xal_index_save({path}); err(-{ESTALE})" in out


def _log_head(cijoe, dev):
    out = _run(cijoe, f"sudo xfs_logprint -t {dev} 2>&1 | grep 'head:'")
    return int(re.search(r"head: (\d+)", out).group(1))


def test_saved_log_wrapped(cijoe):
    """A clean log that has wrapped around its end is read as such."""

    img = IMG + ".wrapped"
    dev = _make(cijoe, img, True)
    try:
        path = _index(cijoe, "wrapped")
        heads = [_log_head(cijoe, dev)]
        # A round of 50,000 files writes about two fifths of the smallest log mkfs makes (64 MiB)
        while len(heads) < 2 or (heads[-1] >= heads[-2] and len(heads) < 10):
            _change(cijoe, dev, "sh -c 'mkdir w && cd w && seq 50000 | xargs touch && cd .. && rm -r w'")
            heads.append(_log_head(cijoe, dev))
        assert heads[-1] < heads[-2], f"the head did not wrap: {heads}"

        expected, _ = _xal(cijoe, dev, "wrapped-reference")
        _, log = _xal(cijoe, dev, "wrapped-save", f"--index-file {path}")
        assert _calls(log, path, ENOENT, 0)
        got, log = _xal(cijoe, dev, "wrapped-load", f"--index-file {path}")
        assert _calls(log, path, 0)
        assert got == expected
    finally:
        cijoe.run(f"sudo umount {MNT}; sudo losetup -d {dev}; rm -f {img}")


def test_saved_offline_tools(cijoe, fs):
    """xfs_admin writes the superblock, which is seen; xfs_db -x writing an inode is not."""

    path = _index(cijoe, "offline")
    _xal(cijoe, fs, "offline-save", f"--index-file {path}")

    _run(cijoe, f"sudo xfs_admin -L xalsaved {fs}")
    _, log = _xal(cijoe, fs, "offline-admin", f"--index-file {path}")
    assert _calls(log, path, ESTALE, 0)

    # The limit xal_index_save() documents: the key does not cover the inodes
    _run(cijoe, f"sudo xfs_db -x -c 'path /d/f0' -c 'write core.size 4096' {fs}")
    _, log = _xal(cijoe, fs, "offline-db", f"--index-file {path}")
    assert _calls(log, path, 0)


def test_saved_fresh_mkfs(cijoe):
    """Right after mkfs the log holds only the record xfsprogs writes, which tells nothing."""

    img = IMG + ".fresh"
    dev = _make(cijoe, img, False)
    try:
        path = _index(cijoe, "fresh")
        _, log = _xal(cijoe, dev, "fresh", f"--index-file {path}")
        assert _calls(log, path, ENOENT, ENODATA)
    finally:
        cijoe.run(f"sudo losetup -d {dev}; rm -f {img}")


def test_saved_subtree(cijoe, fs):
    """An index of a subtree is loaded for that subtree only."""

    path = _index(cijoe, "subtree")
    expected, _ = _xal(cijoe, fs, "subtree-reference", "--subtree /d")

    _, log = _xal(cijoe, fs, "subtree-save", f"--subtree /d --index-file {path}")
    assert _calls(log, path, ENOENT, 0)
    got, log = _xal(cijoe, fs, "subtree-load", f"--subtree /d --index-file {path}")
    assert _calls(log, path, 0)
    assert got == expected

    _, log = _xal(cijoe, fs, "subtree-whole", f"--index-file {path}")
    assert _calls(log, path, ESTALE, 0)


def test_saved_damaged(cijoe, fs):
    """A file cut short or with a changed byte is refused, not read."""

    path = _index(cijoe, "damaged")
    expected, _ = _xal(cijoe, fs, "damaged-save", f"--index-file {path}")

    _run(cijoe, f"cp {path} {path}.copy && truncate -s -1 {path}")
    got, log = _xal(cijoe, fs, "damaged-truncated", f"--index-file {path}")
    assert _calls(log, path, EPROTO, 0)
    assert got == expected

    _run(cijoe, f"cp {path}.copy {path} && chmod 600 {path}")
    flip = "import sys; b = bytearray(open(sys.argv[1], 'rb').read()); b[-3] ^= 0xff; "
    flip += "open(sys.argv[1], 'wb').write(b)"
    _run(cijoe, f'python3 -c "{flip}" {path}')
    got, log = _xal(cijoe, fs, "damaged-byte", f"--index-file {path}")
    assert _calls(log, path, EPROTO, 0)
    assert got == expected


def test_saved_permissions(cijoe, fs):
    """A file others can write, or a symlink, is refused."""

    path = _index(cijoe, "permissions")
    _xal(cijoe, fs, "permissions-save", f"--index-file {path}")

    _run(cijoe, f"chmod 666 {path}")
    _, log = _xal(cijoe, fs, "permissions-writable", f"--index-file {path}")
    assert _calls(log, path, EPERM, 0)

    link = Path(str(path) + ".link")
    _run(cijoe, f"chmod 600 {path} && ln -sf {path} {link}")
    _, log = _xal(cijoe, fs, "permissions-symlink", f"--index-file {link}")
    assert _calls(log, link, ELOOP, 0)
