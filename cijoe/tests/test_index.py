import logging as log
import re
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

    assert indexes["find"] == indexes["xal"]


def test_compare_to_find(cijoe):

    compare_to_find(cijoe, "xal_find.output")


def test_btree_without_attr_fork(cijoe):
    """
    provoke_odf_fmt_btree_no_attr_fork() must give a directory and a file a btree root and no attr
    fork; test_compare_to_find() and test_compare_to_xfs_bmap() then check what xal reads of them
    """

    dev_path = cijoe.getconf("xal.dev_path", None)
    artifacts = Path(cijoe.getconf("xal.artifacts.path"))

    for name in [
        "/should-be-dir-fmt_btree-no_attr_fork",
        "/should-be-file-fmt_btree-no_attr_fork/fragmented-nextents-100",
    ]:
        out = artifacts / "no_attr_fork.xfs_db"
        err, state = cijoe.run(
            f"xfs_db -r -c 'path {name}' -c 'print core.format core.forkoff' {dev_path} > {out}"
        )
        assert not err, f"xfs_db failed on {name}"
        fields = dict(re.findall(r"(\S+) = (\S+)", out.read_text()))
        assert fields.get("core.format") == "3", f"{name} is not in btree format: {fields}"
        assert fields.get("core.forkoff") == "0", f"{name} has an attr fork: {fields}"
