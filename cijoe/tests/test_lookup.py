from contextlib import contextmanager

DEEP_DIR = "/".join(
    f"deeply-nested-directory-with-a-descriptive-name-{cur}" for cur in range(1, 7)
)
DEEP_FILE = f"{DEEP_DIR}/file-rand-4K-0-2.bin"


@contextmanager
def mounted(cijoe):
    """Mount the device for the duration of the block, then unmount it again"""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, _ = cijoe.run(f"sudo mount {dev_path} {mountpoint}")
    assert not err

    try:
        yield mountpoint
    finally:
        cijoe.run(f"sudo umount {mountpoint}")


def lookup(cijoe, path: str, args: str = ""):
    """Have 'xal' resolve 'path', returning the exit status of the tool"""

    dev_path = cijoe.getconf("xal.dev_path", None)

    err, _ = cijoe.run(f"xal {args} --filename {path} {dev_path}")

    return err


def test_deep_path_xfs(cijoe):
    """
    Resolve a path longer than 255 bytes on the XFS backend

    '--backend xfs' is explicit because '--filename' alone selects FIEMAP.
    """

    assert not lookup(cijoe, f"/{DEEP_FILE}", "--backend xfs")


def test_deep_path_fiemap_traverse(cijoe):
    """
    The same lookup on FIEMAP, walking the tree, which is the default lookup mode

    A watch mode is asked for because the alternative freezes the filesystem with FIFREEZE,
    which needs CAP_SYS_ADMIN.
    """

    with mounted(cijoe) as mountpoint:
        assert not lookup(
            cijoe,
            f"{mountpoint}/{DEEP_FILE}",
            "--backend fiemap --watch-mode dirty",
        )


def test_deep_path_fiemap_hashmap(cijoe):
    """The same lookup on FIEMAP, answered from the path-to-inode map"""

    with mounted(cijoe) as mountpoint:
        assert not lookup(
            cijoe,
            f"{mountpoint}/{DEEP_FILE}",
            "--backend fiemap --watch-mode dirty --file_lookup_map",
        )


def test_root_xfs(cijoe):
    """Resolve the root of the indexed tree, given as "/" on the XFS backend"""

    assert not lookup(cijoe, "/", "--backend xfs")


def test_root_fiemap(cijoe):
    """The same on FIEMAP, where the root is the mountpoint and the modes must agree"""

    with mounted(cijoe) as mountpoint:
        for args in [
            "--backend fiemap --watch-mode dirty",
            "--backend fiemap --watch-mode dirty --file_lookup_map",
        ]:
            assert not lookup(cijoe, mountpoint, args)
            assert not lookup(cijoe, f"{mountpoint}/", args)
