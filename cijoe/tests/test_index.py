import logging as log
from pprint import pprint
from pathlib import Path


def compare_to_find(cijoe, filename: str, args: str = ""):
    """Have 'xal' produce a 'find-like' index and compare it to the one from 'find'"""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    paths = {
        "find": Path(cijoe.getconf("xal.artifacts.path")) / "find.output",
        "xal": Path(cijoe.getconf("xal.artifacts.path")) / filename,
    }
    indexes = {
        "find": [],
        "xal": [],
    }

    # Have 'xal' produce the 'find-like' index
    err, state = cijoe.run(f"xal {args} --find {dev_path} > {paths['xal']}")
    assert not err

    for key, path in paths.items():
        for line in sorted(paths[key].read_text().splitlines()):
            indexes[key].append(
                line.replace(dev_path if key == "xal" else mountpoint, "")
            )

    diffs = []
    for expected, got in zip(indexes["find"], indexes["xal"]):
        if expected == got:
            continue

        diffs.append({"expected": expected, "got": got})

    assert not diffs


def test_compare_to_find(cijoe):

    compare_to_find(cijoe, "xal_find.output")


def test_compare_to_find_fiemap(cijoe):
    """
    The same comparison, with the filesystem mounted, that is, on the FIEMAP backend

    A watch mode is asked for because the alternative freezes the filesystem with FIFREEZE,
    which needs CAP_SYS_ADMIN.
    """

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, state = cijoe.run(f"sudo mount {dev_path} {mountpoint}")
    assert not err

    try:
        compare_to_find(
            cijoe, "xal_find_fiemap.output", "--backend fiemap --watch-mode dirty"
        )
    finally:
        cijoe.run(f"sudo umount {mountpoint}")
