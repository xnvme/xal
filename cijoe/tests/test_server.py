"""
Tests for xal-server.

They start ``xal-server`` on the mounted filesystem with a config written for the test, and drive
it with the ``xal_test_mark_dirty`` helper, a reader attached with ``xal_from_shm()``. They need the
filesystem mounted from ``xal.dev_path`` and xal-server installed, and skip otherwise.
"""

import time
from pathlib import Path

import pytest

SHM_NAME = "/xal_test_server"


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    out = _artifacts(cijoe) / "server_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _require_server(cijoe):
    err, _ = cijoe.run("command -v xal-server")
    if err:
        pytest.skip("xal-server is not installed")


def _require_mount(cijoe):
    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    if src != dev_path:
        pytest.skip(f"{mountpoint} is not mounted from {dev_path}")
    return dev_path, mountpoint


def _wait_for(cijoe, cmd, want_ok, seconds):
    for _ in range(seconds * 10):
        err, _ = cijoe.run(cmd)
        if (not err) == want_ok:
            return True
        time.sleep(0.1)
    return False


def _start_server(cijoe, dev_path, mountpoint, subtree=None):
    """Start xal-server in XAL_WATCHMODE_NONE on the mount; return its pid once it published"""

    art = _artifacts(cijoe)
    conf = art / "xal-server.conf"
    log = art / "xal-server.log"
    pid_path = art / "xal-server.pid"
    sub = f', subtree = "{subtree}"' if subtree else ""

    conf.write_text(
        "log_level = 3\n"
        f'devices = [ {{ uri = "{dev_path}", shm_name = "{SHM_NAME}", '
        f'mountpoint = "{mountpoint}"{sub} }} ]\n'
        "\n[xal]\nwatchmode = 0\n"
    )
    cijoe.run(f"rm -f {pid_path} /dev/shm{SHM_NAME}_*")
    cijoe.run(
        f"sh -c 'nohup xal-server --config {conf} > {log} 2>&1 & echo $! > {pid_path}'"
    )
    pid = pid_path.read_text().strip()
    assert pid, "xal-server did not start"

    if not _wait_for(cijoe, f"test -e /dev/shm{SHM_NAME}_state", True, 30):
        cijoe.run(f"kill {pid}")
        journal = _capture(cijoe, f"sudo journalctl -q --no-pager -t xal-server _PID={pid}")
        raise AssertionError(
            f"xal-server published nothing\nstderr: {log.read_text()}\njournal: {journal}"
        )
    return pid


def _stop_server(cijoe, pid):
    cijoe.run(f"kill {pid}")
    assert _wait_for(cijoe, f"test -e /dev/shm{SHM_NAME}_state", False, 20), (
        "xal-server left its regions behind"
    )


def test_none_reindexes_on_a_readers_mark(cijoe):
    """In XAL_WATCHMODE_NONE, a file created after start-up is indexed once a reader marks the
    index dirty."""

    _require_server(cijoe)
    dev_path, mountpoint = _require_mount(cijoe)
    new = f"{mountpoint}/xal_server_new"

    cijoe.run(f"rm -f {new}")
    pid = _start_server(cijoe, dev_path, mountpoint)
    try:
        err, _ = cijoe.run(f"xal_test_mark_dirty {SHM_NAME} {new} 5000")
        assert not err, "the reader's mark did not get the new file indexed"
    finally:
        _stop_server(cijoe, pid)
        cijoe.run(f"rm -f {new}")


def test_none_retries_a_failed_reindex(cijoe):
    """In XAL_WATCHMODE_NONE, a re-index that fails, here because the subtree is gone, is tried
    again until it succeeds."""

    _require_server(cijoe)
    dev_path, mountpoint = _require_mount(cijoe)
    subtree = f"{mountpoint}/xal_server_sub"
    new = f"{subtree}/new"

    err, _ = cijoe.run(f"rm -rf {subtree} {subtree}.away && mkdir {subtree}")
    assert not err
    pid = _start_server(cijoe, dev_path, mountpoint, subtree)
    try:
        err, _ = cijoe.run(f"xal_test_mark_dirty {SHM_NAME} {new} 10000 {subtree}")
        assert not err, "a failed re-index was not retried into one with the new file"
    finally:
        _stop_server(cijoe, pid)
        cijoe.run(f"rm -rf {subtree} {subtree}.away")
