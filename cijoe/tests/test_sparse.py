"""
Tests for a filesystem whose inode chunks are sparse: allocated four or more inodes at a time
where no space for a whole chunk is left, so that the inobt records have holes (holemask)

The test makes its own filesystem on a loop device, as the xfstests sparse inode tests do (e.g.
xfs/076): it preallocates the free space in one file and punches holes into it that hold a
cluster of 32 inodes wherever they land but never a whole chunk of 64, then creates 5,000 inodes.
The filesystem has no btrees that reserve space in each AG (rmapbt, reflink, finobt) and no
reserved blocks, so that no free space outside the holes is left for whole chunks.
"""

import re

IMG = "/tmp/xal_sparse.img"
MNT = "/tmp/xal_sparse_mnt"


def _run(cijoe, cmd):
    err, state = cijoe.run(cmd)
    assert not err, cmd
    return state.output()


def _holemasks(cijoe, dev):
    """The holemask of every inobt record of every AG"""

    agcount = int(re.search(r"agcount = (\d+)", _run(cijoe, f"sudo xfs_db -r -c 'sb 0' -c 'p agcount' {dev}")).group(1))
    holemasks = []
    for agno in range(agcount):
        out = _run(cijoe, f"sudo xfs_db -r -c 'agi {agno}' -c 'addr root' -c btdump {dev}")
        holemasks += [int(m, 0) for m in re.findall(r"\d+:\[\d+,(0x[0-9a-f]+|\d+),", out)]
    return holemasks


def test_sparse_chunks(cijoe):
    """The index of a filesystem with sparse inode chunks is the one 'find' gives."""

    artifacts = cijoe.getconf("xal.artifacts.path")

    _run(cijoe, f"rm -f {IMG} && truncate -s 512M {IMG}")
    dev = _run(cijoe, f"sudo losetup -f --show {IMG}").strip().splitlines()[-1]
    try:
        _run(cijoe, f"sudo chown $USER {dev}")
        # mkfs refuses a new filesystem smaller than 300 MB
        _run(
            cijoe,
            f"sudo mkfs.xfs -f -q -d size=320m -m rmapbt=0,reflink=0,finobt=0 "
            f"-i sparse=1,size=512 {dev}",
        )
        _run(cijoe, f"sudo mkdir -p {MNT} && sudo mount {dev} {MNT}")
        try:
            # Preallocate the free space but 16 blocks, then punch 28 KiB of every 32 KiB. A
            # 16 KiB cluster on a 16 KiB boundary fits in each hole, a 32 KiB chunk in none.
            _run(cijoe, f"sudo xfs_io -x -c 'resblks 0' {MNT}")
            avail = int(_run(cijoe, f"df --output=avail -B4096 {MNT} | tail -1"))
            size = (avail - 16) * 4096
            _run(cijoe, f"sudo xfs_io -f -c 'falloc 0 {size}' {MNT}/big")
            _run(cijoe, f"seq -f 'fpunch %.0f 28k' 0 32768 {size - 1} | sudo xfs_io {MNT}/big")
            cijoe.run(f"sudo mkdir {MNT}/i && cd {MNT}/i && seq 5000 | sudo xargs touch")
            _run(cijoe, f"sync && find {MNT} | sort > {artifacts}/sparse-find.output")
        finally:
            _run(cijoe, f"sudo umount {MNT}")

        freesp = _run(cijoe, f"sudo xfs_db -r -c 'freesp -s' {dev}")
        assert any(_holemasks(cijoe, dev)), f"no inobt record has a hole; {freesp}"

        expected = (
            open(f"{artifacts}/sparse-find.output").read().replace(MNT, "").splitlines()
        )
        out = _run(cijoe, f"xal --find {dev} > {artifacts}/sparse-xal.output && echo done")
        assert "done" in out
        got = sorted(
            open(f"{artifacts}/sparse-xal.output").read().replace(dev, "").splitlines()
        )
        assert len(got) > 5000
        assert got == expected
    finally:
        cijoe.run(f"sudo umount {MNT}; sudo losetup -d {dev}; rm -f {IMG}")
