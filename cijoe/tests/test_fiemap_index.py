"""
Tests for the index the FIEMAP backend builds of the populated filesystem.

They repeat the 'find', 'xfs_bmap' and lookup comparisons of the XFS backend on the FIEMAP backend.
They need the populated filesystem mounted from ``xal.dev_path``, as the second phase of
prep_and_test.yaml leaves it, and skip otherwise.
"""

import errno
from pathlib import Path

import pytest

from test_bmap import compare_to_xfs_bmap
from test_index import compare_to_find
from test_lookup import DEEP_FILE, lookup


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    out = _artifacts(cijoe) / "fiemap_index_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _require_mount(cijoe):
    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    if src != dev_path:
        pytest.skip(f"{mountpoint} is not mounted from {dev_path}")
    return dev_path, mountpoint


def test_compare_to_find(cijoe):
    """The index 'xal --find' prints matches the one from 'find'"""

    _require_mount(cijoe)

    compare_to_find(cijoe, "xal_find_fiemap.output", "--backend fiemap")


def test_compare_to_xfs_bmap(cijoe):
    """The extents 'xal --bmap' prints match those from 'xfs_bmap'"""

    _require_mount(cijoe)

    compare_to_xfs_bmap(cijoe, "xal_bmap_fiemap.yaml", "--backend fiemap")


def test_deep_path_traverse(cijoe):
    """Resolve a path longer than 255 bytes by walking the tree, the default lookup mode"""

    _, mountpoint = _require_mount(cijoe)

    assert not lookup(cijoe, f"{mountpoint}/{DEEP_FILE}", "--backend fiemap")


def test_deep_path_hashmap(cijoe):
    """The same lookup, answered from the path-to-inode map"""

    _, mountpoint = _require_mount(cijoe)

    assert not lookup(
        cijoe, f"{mountpoint}/{DEEP_FILE}", "--backend fiemap --file_lookup_map"
    )


def test_root(cijoe):
    """Resolve the root of the indexed tree, the mountpoint, where the lookup modes must agree"""

    _, mountpoint = _require_mount(cijoe)

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert not lookup(cijoe, mountpoint, args)
        assert not lookup(cijoe, f"{mountpoint}/", args)


def test_empty_path(cijoe):
    """An empty path names no entry, in either lookup mode"""

    _require_mount(cijoe)

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, "", args) == errno.EINVAL


def test_basepath_prefix(cijoe):
    """A path that only shares a prefix with the mountpoint is not under it, in either mode"""

    _, mountpoint = _require_mount(cijoe)

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, f"{mountpoint}X/{DEEP_FILE}", args) == errno.EINVAL


def test_missing(cijoe):
    """A path to an entry that does not exist is not found, the same way in both lookup modes"""

    _, mountpoint = _require_mount(cijoe)

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, f"{mountpoint}/does-not-exist", args) == errno.ENOENT
