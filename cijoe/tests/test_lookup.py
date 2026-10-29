import errno
import shlex

DEEP_DIR = "/".join(
    f"deeply-nested-directory-with-a-descriptive-name-{cur}" for cur in range(1, 7)
)
DEEP_FILE = f"{DEEP_DIR}/file-rand-4K-0-2.bin"


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


def test_root_xfs(cijoe):
    """Resolve the root of the indexed tree, given as "/" on the XFS backend"""

    assert not lookup(cijoe, "/", "--backend xfs")


def test_empty_path_xfs(cijoe):
    """An empty path names no entry"""

    assert lookup(cijoe, "", "--backend xfs")


def test_missing_xfs(cijoe):
    """A path to an entry that does not exist is not found"""

    assert lookup(cijoe, "/does-not-exist", "--backend xfs") == errno.ENOENT
