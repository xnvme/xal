"""
Tests for the index the FIEMAP backend builds of the populated filesystem.

They repeat the 'find', 'xfs_bmap' and lookup comparisons of the XFS backend on the FIEMAP backend.
They need the populated filesystem mounted from ``xal.dev_path``, as the second phase of
prep_and_test.yaml leaves it, and skip otherwise.
"""

import errno

from test_bmap import compare_to_xfs_bmap
from test_index import compare_to_find
from test_lookup import DEEP_FILE, LONG_NAME_FILE, lookup


def test_compare_to_find(cijoe, require_mount):
    """The index 'xal --find' prints matches the one from 'find'"""

    compare_to_find(cijoe, "xal_find_fiemap.output", "--backend fiemap")


def test_compare_to_xfs_bmap(cijoe, require_mount):
    """The extents 'xal --bmap' prints match those from 'xfs_bmap'"""

    compare_to_xfs_bmap(cijoe, "xal_bmap_fiemap.yaml", "--backend fiemap")


def test_deep_path_traverse(cijoe, require_mount):
    """Resolve a path longer than 255 bytes by walking the tree, the default lookup mode"""

    _, mountpoint = require_mount

    assert not lookup(cijoe, f"{mountpoint}/{DEEP_FILE}", "--backend fiemap")


def test_deep_path_hashmap(cijoe, require_mount):
    """The same lookup, answered from the path-to-inode map"""

    _, mountpoint = require_mount

    assert not lookup(
        cijoe, f"{mountpoint}/{DEEP_FILE}", "--backend fiemap --file_lookup_map"
    )


def test_long_name_traverse(cijoe, require_mount):
    """Resolve a file whose name is the maximum 255 bytes by walking the tree"""

    _, mountpoint = require_mount

    assert not lookup(cijoe, f"{mountpoint}/{LONG_NAME_FILE}", "--backend fiemap")


def test_long_name_hashmap(cijoe, require_mount):
    """The same lookup, answered from the path-to-inode map"""

    _, mountpoint = require_mount

    assert not lookup(
        cijoe, f"{mountpoint}/{LONG_NAME_FILE}", "--backend fiemap --file_lookup_map"
    )


def test_root(cijoe, require_mount):
    """Resolve the root of the indexed tree, the mountpoint, where the lookup modes must agree"""

    _, mountpoint = require_mount

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert not lookup(cijoe, mountpoint, args)
        assert not lookup(cijoe, f"{mountpoint}/", args)


def test_empty_path(cijoe, require_mount):
    """An empty path names no entry, in either lookup mode"""

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, "", args) == errno.EINVAL


def test_basepath_prefix(cijoe, require_mount):
    """A path that only shares a prefix with the mountpoint is not under it, in either mode"""

    _, mountpoint = require_mount

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, f"{mountpoint}X/{DEEP_FILE}", args) == errno.EINVAL


def test_missing(cijoe, require_mount):
    """A path to an entry that does not exist is not found, the same way in both lookup modes"""

    _, mountpoint = require_mount

    for args in ["--backend fiemap", "--backend fiemap --file_lookup_map"]:
        assert lookup(cijoe, f"{mountpoint}/does-not-exist", args) == errno.ENOENT
