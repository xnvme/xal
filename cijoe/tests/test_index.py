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


def test_dir_btree_leaves(cijoe):
    """
    The directory made by provoke_odf_dir_fmt_btree_leaves() has an extent B+tree with more than
    one leaf; every one of its entries must be in the index
    """

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)
    artifacts = Path(cijoe.getconf("xal.artifacts.path"))
    name = "/should-be-dir-fmt_btree-leaves"

    # Without a root of more than one record the directory no longer provokes what this tests
    err, state = cijoe.run(
        f"xfs_db -r -c 'path {name}' "
        f"-c 'print core.format core.forkoff core.nextents u3.bmbt.numrecs' {dev_path}"
        f" > {artifacts / 'btree_leaves.xfs_db'}"
    )
    assert not err, f"xfs_db failed on {name}"
    fields = dict(re.findall(r"(\S+) = (\S+)", (artifacts / "btree_leaves.xfs_db").read_text()))
    assert fields.get("core.format") == "3", f"{name} is not in btree format: {fields}"
    numrecs = int(fields["u3.bmbt.numrecs"])
    assert numrecs > 1, f"the btree root of {name} holds {numrecs} record; it needs two: {fields}"

    err, state = cijoe.run(f"xal --find {dev_path} > {artifacts / 'xal_find_leaves.output'}")
    assert not err

    expected = [
        line[len(mountpoint) :]
        for line in (artifacts / "find.output").read_text().splitlines()
        if line.startswith(f"{mountpoint}{name}/")
    ]
    got = [
        line[len(dev_path) :]
        for line in (artifacts / "xal_find_leaves.output").read_text().splitlines()
        if line.startswith(f"{dev_path}{name}/")
    ]
    assert len(expected) == 10000, "provoke_odf_dir_fmt_btree_leaves() makes 10000 files"
    assert len(got) == len(expected), f"xal lists {len(got)} of the {len(expected)} entries"
    assert sorted(got) == sorted(expected)


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
