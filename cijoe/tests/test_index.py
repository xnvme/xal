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

    assert indexes["find"] == indexes["xal"]


def test_compare_to_find(cijoe):

    compare_to_find(cijoe, "xal_find.output")
