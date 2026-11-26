import pytest


@pytest.fixture
def require_mount(cijoe):
    """Skip the test when the device is not mounted; give its path and mountpoint otherwise."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, _ = cijoe.run(f"findmnt --source {dev_path} --mountpoint {mountpoint} > /dev/null")
    if err:
        pytest.skip(f"{mountpoint} is not mounted from {dev_path}")

    return dev_path, mountpoint
