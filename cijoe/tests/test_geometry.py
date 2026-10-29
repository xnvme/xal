import re
from pathlib import Path

import pytest


@pytest.mark.parametrize("mkfs_opts", ["-i size=2048", "-b size=65536"])
def test_inode_chunk_geometry(cijoe, mkfs_opts):
    """2 KiB inodes make 128 KiB chunks, and 64 KiB blocks put two chunks in one block"""

    name = "geometry" + re.sub(r"\W", "", mkfs_opts)
    tmp = Path(cijoe.getconf("xal.artifacts.path")) / name
    files = [f"f{i:03d}" for i in range(200)]
    tmp.with_suffix(".data").write_bytes(b"x" * 5000)
    proto = ["/dev/null", "0 0", "d--755 0 0", "d d--755 0 0"]
    proto += [f"{f} ---644 0 0 {tmp}.data" for f in files] + ["$", "$", ""]
    tmp.with_suffix(".proto").write_text("\n".join(proto))

    dev = None
    try:
        # The protofile fills the file system, so no kernel has to mount 64 KiB blocks
        err, _ = cijoe.run(f"truncate -s 512M {tmp}.img")
        assert not err
        err, _ = cijoe.run(f"mkfs.xfs -f {mkfs_opts} -p {tmp}.proto {tmp}.img")
        assert not err
        err, state = cijoe.run(f"xfs_db -r -c 'sb 0' -c 'p inodesize inopblock icount' {tmp}.img")
        assert not err
        sb = {k: int(v) for k, v in re.findall(r"(\w+) = (\d+)", state.output())}
        assert sb["icount"] > 64 and (sb["inodesize"] == 2048 or sb["inopblock"] > 64), sb

        err, state = cijoe.run(f"sudo losetup -f --show {tmp}.img")
        assert not err
        dev = state.output().split()[-1]
        err, _ = cijoe.run(f"sudo chown $USER {dev}")
        assert not err

        err, state = cijoe.run(f"xal --find {dev}")
        assert not err
        got = sorted(l[len(dev) :] for l in state.output().splitlines() if l.startswith(dev))
        assert got == sorted(["", "/d"] + [f"/d/{f}" for f in files])

        err, state = cijoe.run(f"xal --bmap {dev}")
        assert not err
        assert sorted(re.findall(r"/d/(f\d+)':\n- \[", state.output())) == files
    finally:
        if dev:
            cijoe.run(f"sudo losetup -d {dev}")
        cijoe.run(f"rm -f {tmp}.img")
