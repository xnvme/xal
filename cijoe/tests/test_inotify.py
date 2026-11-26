def test_inotify_on_xfs(cijoe, require_mount):
    """Run xal_test_inotify against the XFS mountpoint, as root for its unmount case"""

    _, mountpoint = require_mount

    err, state = cijoe.run(f"sudo xal_test_inotify {mountpoint}")
    assert not err, state.output()
