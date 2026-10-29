import logging as log
from pprint import pprint
from pathlib import Path
import json
import yaml


def compare_to_xfs_bmap(cijoe, filename: str, args: str = ""):
    """Have 'xal' produce a bmap and compare it to the one from 'xfs_bmap'"""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)
    artifacts_path = Path(cijoe.getconf("xal.artifacts.path"))

    def convert_xal_bmap(artifacts_path: Path):
        """Produce xal_bmap via 'xal --bmap ...' and normalize the output"""

        xal_bmap_path = artifacts_path / filename

        err, state = cijoe.run(f"xal {args} --bmap {dev_path} > {xal_bmap_path}")
        assert not err

        xal_bmap = {}
        for key, values in yaml.safe_load(xal_bmap_path.read_text()).items():
            xal_bmap[key.replace(dev_path, "")] = values if values else []

        got_bmap = artifacts_path / f"got_{filename}"
        got_bmap.write_text(yaml.safe_dump(xal_bmap))

        return got_bmap

    def convert_xfs_bmap(artifacts_path: Path):

        xfs_bmap_path = artifacts_path / "bmap.json"

        xfs_bmap = {}
        for key, values in json.loads(xfs_bmap_path.read_text()).items():
            ino, extents = values
            xfs_bmap[key.replace(mountpoint, "")] = extents if extents else []

        expected_bmap = artifacts_path / "expected_bmap.yaml"
        expected_bmap.write_text(yaml.safe_dump(xfs_bmap))

        return expected_bmap

    expected_bmap = convert_xfs_bmap(artifacts_path)
    got_bmap = convert_xal_bmap(artifacts_path)

    assert yaml.safe_load(expected_bmap.read_text()) == yaml.safe_load(
        got_bmap.read_text()
    )


def test_compare_to_xfs_bmap(cijoe):

    compare_to_xfs_bmap(cijoe, "xal_bmap.yaml")
