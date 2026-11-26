import errno
import shlex

DEEP_DIR = "/".join(
    f"deeply-nested-directory-with-a-descriptive-name-{cur}" for cur in range(1, 7)
)
DEEP_FILE = f"{DEEP_DIR}/file-rand-4K-0-2.bin"
LONG_NAME_FILE = "should-be-dir-fmt_btree-no_attr_fork/long-name-000000-" + "a" * 238


def lookup(cijoe, path: str, args: str = ""):
    """Have 'xal' resolve 'path', returning the exit status of the tool"""

    dev_path = cijoe.getconf("xal.dev_path", None)

    err, _ = cijoe.run(f"xal {args} --filename {shlex.quote(path)} {dev_path}")

    return err


def test_deep_path_xfs(cijoe):
    """
    Resolve a path longer than 255 bytes on the XFS backend

    '--backend xfs' is explicit because '--filename' alone selects FIEMAP.
    """

    assert not lookup(cijoe, f"/{DEEP_FILE}", "--backend xfs")


def test_long_name_xfs(cijoe):
    """Resolve a file whose name is the maximum 255 bytes on the XFS backend"""

    assert not lookup(cijoe, f"/{LONG_NAME_FILE}", "--backend xfs")


def test_root_xfs(cijoe):
    """Resolve the root of the indexed tree, given as "/" on the XFS backend"""

    assert not lookup(cijoe, "/", "--backend xfs")


def test_empty_path_xfs(cijoe):
    """An empty path names no entry"""

    assert lookup(cijoe, "", "--backend xfs")


def test_missing_xfs(cijoe):
    """A path to an entry that does not exist is not found"""

    assert lookup(cijoe, "/does-not-exist", "--backend xfs") == errno.ENOENT


def test_lookup_after_reindex(cijoe, require_mount):
    """
    Run the C integration test for lookups across a re-index against the XFS mountpoint: it
    changes a directory tree between two calls to xal_index() and checks that lookups, by
    traversal and through the hash map, follow the change.
    """

    _, mountpoint = require_mount

    err, state = cijoe.run(f"xal_test_lookup_map {mountpoint}")
    assert not err, state.output()
