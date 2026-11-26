"""
Tests for sharing an index across processes over POSIX shared memory.

test_shm_version_skew drives the ``xal_test_shm_version`` helper, which does the whole check in
one process: it opens a primary with ``opts.shm_name``, indexes, and attaches to its own published
regions with ``xal_from_shm()``: once as published, once with the version in the state region
edited, and once with it put back.

test_shm_primary_secondary runs the C integration tests, which open a primary on the mounted
device and attach secondaries with xal_from_shm().
"""

SHM_NAME = "xal_test_shm_version"


def test_shm_version_skew(cijoe):
    """A state region published at another version must be refused, not misread."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    # The regions are created O_EXCL and outlive a primary that died without unlinking, so a
    # remnant of an earlier run would fail the open with -EEXIST rather than be reused.
    cijoe.run(f"rm -f /dev/shm/{SHM_NAME}_*")

    err, _ = cijoe.run(f"xal_test_shm_version {dev_path} {mountpoint}")
    assert not err


def test_shm_primary_secondary(cijoe, require_mount):
    """Run xal_test_shm on the mounted device: secondaries from xal_from_shm() share the
    primary's index"""

    dev_path, _ = require_mount

    err, state = cijoe.run(f"xal_test_shm {dev_path}")
    assert not err, state.output()
