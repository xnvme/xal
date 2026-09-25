import pytest


def _is_mounted(cijoe) -> bool:
    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, _ = cijoe.run(f"findmnt --source {dev_path} --mountpoint {mountpoint} > /dev/null")
    return not err


@pytest.fixture
def require_mount(cijoe):
    """Skip the test when the device is not mounted."""

    if not _is_mounted(cijoe):
        pytest.skip("the test requires the device to be mounted")
