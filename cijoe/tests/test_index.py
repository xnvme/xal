import logging as log
import re
from pprint import pprint
from pathlib import Path


def test_compare_to_find(cijoe):

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    paths = {
        "find": Path(cijoe.getconf("xal.artifacts.path")) / "find.output",
        "xal": Path(cijoe.getconf("xal.artifacts.path")) / "xal_find.output",
    }
    indexes = {
        "find": [],
        "xal": [],
    }

    # Have 'xal' produce the 'find-like' index
    err, state = cijoe.run(f"xal --find {dev_path} > {paths['xal']}")
    assert not err

    for key, path in paths.items():
        for line in sorted(paths[key].read_text().splitlines()):
            indexes[key].append(
                line.replace(dev_path if key == "xal" else mountpoint, "")
            )

    # zip() below stops at the shorter list, so entries missing at the end would go unseen
    assert len(indexes["xal"]) == len(
        indexes["find"]
    ), f"xal lists {len(indexes['xal'])} entries, find {len(indexes['find'])}"

    diffs = []
    for expected, got in zip(indexes["find"], indexes["xal"]):
        if expected == got:
            continue

        diffs.append({"expected": expected, "got": got})

    assert not diffs


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
