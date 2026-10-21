"""
Tests for indexing a subtree with the XFS backend ('xal --subtree', opts.subtree)

Each runs 'xal' on the unmounted device and compares its output with the part of the 'find' and
'xfs_bmap' output from prep_files that is beneath the same path, the subtree's own path cut off.
"""

import json
from pathlib import Path

import pytest
import yaml

# Directories of each format, and paths of more than one component with directories beneath
SUBTREES = [
    "/should-be-dir-fmt_local",
    "/should-be-dir-fmt_extents-more",
    "/should-be-dir-fmt_btree",
    "/intent_iab3_lvl1",
    "/should-be-file-fmt_btree",
    "/nested",
    "/nested/a",
    "/nested/a/b/c",
    "/should-be-dir-fmt_extents-more/zz-sub",
    "/intent_iab3_lvl1/zz-sub",
]

# The directory format each fixture aims at. How many extents the data section of 5000 entries
# takes varies from run to run, so it is not always too many for the inode, as with 20000.
FORMATS = {
    "local": ["/should-be-dir-fmt_local"],
    "extents": ["/should-be-dir-fmt_extents-more"],
    "btree": ["/should-be-dir-fmt_btree", "/intent_iab3_lvl1"],
}


def _beneath(path, subtree):
    return path == subtree or path.startswith(subtree + "/")


def _xal_find(cijoe, subtree, name):
    dev_path = cijoe.getconf("xal.dev_path", None)
    out = Path(cijoe.getconf("xal.artifacts.path")) / name

    cmd = f"xal --find {dev_path} > {out}"
    if subtree is not None:
        cmd = f"xal --find --subtree '{subtree}' {dev_path} > {out}"
    err, _ = cijoe.run(cmd)
    assert not err

    return sorted(line.replace(dev_path, "", 1) for line in out.read_text().splitlines())


@pytest.mark.parametrize("subtree", SUBTREES)
def test_subtree_find(cijoe, subtree):
    """The subtree's index holds what 'find' lists beneath it, and nothing else."""

    mountpoint = cijoe.getconf("xal.mountpoint", None)
    find = Path(cijoe.getconf("xal.artifacts.path")) / "find.output"

    expected = sorted(
        line[len(mountpoint) + len(subtree) :]
        for line in find.read_text().splitlines()
        if _beneath(line[len(mountpoint) :], subtree)
    )
    got = _xal_find(cijoe, subtree, f"xal_find_subtree{subtree.replace('/', '_')}.output")

    assert len(expected) > 1
    assert got == expected


def test_subtree_formats(cijoe):
    """The subtrees above include a directory of each format, as their fixtures intend."""

    dev_path = cijoe.getconf("xal.dev_path", None)

    for fmt, subtrees in FORMATS.items():
        found = []
        for subtree in subtrees:
            err, state = cijoe.run(
                f"xfs_db -r -c 'path {subtree}' -c 'print core.format' {dev_path}"
            )
            assert not err
            if f"({fmt})" in state.output():
                found.append(subtree)
        assert found, f"none of {subtrees} is in {fmt} format"


@pytest.mark.parametrize("subtree", ["/should-be-file-fmt_btree", "/nested/a"])
def test_subtree_bmap(cijoe, subtree):
    """The extents of the files beneath the subtree are those 'xfs_bmap' reports."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)
    artifacts = Path(cijoe.getconf("xal.artifacts.path"))
    out = artifacts / f"xal_bmap_subtree{subtree.replace('/', '_')}.yaml"

    err, _ = cijoe.run(f"xal --bmap --subtree {subtree} {dev_path} > {out}")
    assert not err

    got = {
        key.replace(dev_path, "", 1): values if values else []
        for key, values in yaml.safe_load(out.read_text()).items()
    }
    expected = {}
    for path, (_, extents) in json.loads((artifacts / "bmap.json").read_text()).items():
        if _beneath(path[len(mountpoint) :], subtree):
            expected[path[len(mountpoint) + len(subtree) :]] = extents if extents else []

    assert expected
    assert got == expected


@pytest.mark.parametrize(
    "subtree,path",
    [
        ("/nested/a", "/nested/a/b/c/file-rand-4K-0-40.bin"),
        ("/intent_iab3_lvl1", "/intent_iab3_lvl1/zz-sub/file-rand-4K-0-3.bin"),
    ],
)
def test_subtree_file(cijoe, subtree, path):
    """A file is looked up by its path from the root of the filesystem, subtree or not."""

    dev_path = cijoe.getconf("xal.dev_path", None)

    outputs = []
    for opts in ["", f"--subtree {subtree}"]:
        err, state = cijoe.run(f"xal --backend xfs {opts} --filename {path} {dev_path}")
        assert not err
        outputs.append(state.output())
    assert outputs[0] == outputs[1]
    assert f"'{path}':" in outputs[1]

    # A path that only starts with the subtree's characters is not beneath it, even when what
    # follows them is a path beneath it
    other = subtree + "X" + path[len(subtree) + 1 :]
    err, state = cijoe.run(
        f"xal --backend xfs --subtree {subtree} --filename {other} {dev_path}"
    )
    assert err
    assert "err(-22)" in state.output()


def test_subtree_root_and_slashes(cijoe):
    """'/' indexes the whole filesystem; repeated and trailing slashes do not matter."""

    assert _xal_find(cijoe, "/", "xal_find_subtree_root.output") == _xal_find(
        cijoe, None, "xal_find_whole.output"
    )
    assert _xal_find(cijoe, "//nested//a/", "xal_find_subtree_slashes.output") == _xal_find(
        cijoe, "/nested/a", "xal_find_subtree_nested_a.output"
    )


@pytest.mark.parametrize(
    "subtree,errno",
    [
        ("/nested/no-such-dir", -2),
        ("/nested/e/file-rand-4K-0-2.bin", -20),
        ("/nested/e/file-rand-4K-0-2.bin/x", -20),
        ("nested", -22),
        ("/nested/../nested", -22),
    ],
)
def test_subtree_refused(cijoe, subtree, errno):
    """A subtree that is not there, not a directory, or not an absolute path is refused."""

    dev_path = cijoe.getconf("xal.dev_path", None)

    err, state = cijoe.run(f"xal --find --subtree '{subtree}' {dev_path}")
    assert err
    assert f"err({errno})" in state.output()
