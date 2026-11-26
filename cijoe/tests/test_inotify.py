def test_inotify_on_xfs(cijoe, require_mount):
    """Run xal_test_inotify against the XFS mountpoint, as root for its unmount case"""

    mountpoint = cijoe.getconf("xal.mountpoint", None)
    assert mountpoint, "xal.mountpoint must be set in the config"

    err, state = cijoe.run(f"sudo xal_test_inotify {mountpoint}")
    assert not err, state.output()
