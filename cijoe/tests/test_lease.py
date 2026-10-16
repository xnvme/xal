"""
Tests for XAL_WATCHMODE_READ_LEASE (the FIEMAP read-lease mode).

These drive the ``xal_lease_hold`` helper, which opens the FIEMAP backend in read-lease mode,
indexes, and holds the extents of one file with xal_extents_acquire() until signalled -- so a test
can open the file for writing meanwhile and check that the writer waits, that the extents still
hold the file's data, and that the file is mapped again afterwards. The helper runs under sudo,
which gives it CAP_LEASE for files the test user owns.
"""

import re
import time
from pathlib import Path

import pytest

READY_RE = re.compile(r"LEASE_READY pid=(\d+) byte=(\d+) extents=(\d+)")
RELEASED_RE = re.compile(r"RELEASED err=(-?\d+)")
RECALLED_RE = re.compile(r"RECALLED left_ms=(-?\d+)")
REACQUIRED_RE = re.compile(r"REACQUIRED byte=(\d+) extents=(\d+)")
EXTENT_RE = re.compile(r"EXTENT 0 byte=(\d+) bytes=(\d+) flag=0x([0-9a-f]+)")
SWEEP_RE = re.compile(
    r"SWEEP files=(\d+) failed=(\d+) err=(-?\d+) leases=(-?\d+) evicted=(\d+)"
)

FIEMAP_EXTENT_DELALLOC = 0x4
FMR_OWN_FREE = "special_0:1"
EINVAL = 22
ENOTSUP = 95
ESTALE = 116


@pytest.fixture(autouse=True)
def _restore_lease_break_time(cijoe):
    """Some tests change the system-wide fs.lease-break-time; restore it even if they fail."""

    saved = _capture(cijoe, "cat /proc/sys/fs/lease-break-time")
    yield
    cijoe.run(f"sudo sysctl -qw fs.lease-break-time={saved}")


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    """Run 'cmd', capturing stdout to an artifact file, and return it stripped."""

    out = _artifacts(cijoe) / "lease_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _require_fixture(cijoe):
    """Resolve (dev_path, mountpoint) and assert the mount comes from dev_path."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    if not src:
        pytest.skip(f"nothing is mounted at {mountpoint}")
    assert src == dev_path, f"{mountpoint} is mounted from {src!r}, not {dev_path!r}"

    return dev_path, mountpoint


def _fill(cijoe, path, char, nbytes, sync=True):
    cijoe.run(f"sh -c \"printf '%{nbytes}s' | tr ' ' {char} > {path}\"")
    if sync:
        cijoe.run("sync")


def _raw(cijoe, dev_path, byte, nbytes=4) -> str:
    """The first nbytes at the device byte offset, read with O_DIRECT past the page cache."""

    assert byte % 4096 == 0, f"byte offset {byte} is not 4 KiB aligned"
    return _capture(
        cijoe,
        f"sudo dd if={dev_path} bs=4096 skip={byte // 4096} count=1 iflag=direct status=none "
        f"| head -c {nbytes}",
    )


def _background(cijoe, name, script) -> Path:
    """Run a shell script in the background; return the path of its log."""

    art = _artifacts(cijoe)
    (art / f"{name}.sh").write_text(script)
    log = art / f"{name}.log"
    cijoe.run(f"rm -f {log}")
    cijoe.run(f"sh -c 'nohup sh {art / name}.sh > {log} 2>&1 &'")
    return log


def _wait_for(path: Path, regex, timeout):
    for _ in range(int(timeout / 0.1)):
        if path.exists():
            m = regex.search(path.read_text())
            if m:
                return m
        time.sleep(0.1)
    text = path.read_text() if path.exists() else "(no log)"
    raise AssertionError(f"{regex.pattern} not seen in {path} within {timeout}s:\n{text}")


def _start_hold(cijoe, dev_path, mountpoint, path, option=""):
    """Start xal_lease_hold on path; return (pid, byte, log) once it holds the extents."""

    log = _artifacts(cijoe) / "lease_hold.log"
    cijoe.run(f"sudo rm -f {log}")
    cijoe.run(
        f"sudo sh -c 'nohup xal_lease_hold {dev_path} {mountpoint} {path} {option} > {log} 2>&1 & "
        f"echo backgrounded'"
    )
    m = _wait_for(log, READY_RE, 30)
    return int(m.group(1)), int(m.group(2)), log


def _stop_hold(cijoe, pid):
    cijoe.run(f"sudo kill {pid}")
    for _ in range(100):
        err, _ = cijoe.run(f"sudo kill -0 {pid}")
        if err:
            return
        time.sleep(0.1)
    raise AssertionError(f"xal_lease_hold pid {pid} did not exit")


def _probe(cijoe, dev_path, mountpoint, path) -> str:
    return _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {path} --probe")


def _timed(cijoe, name, cmd) -> Path:
    """Run cmd in the background; its log gets took_ms=<ms> once it is done."""

    return _background(
        cijoe,
        name,
        f"start=$(date +%s%N)\n{cmd}\necho took_ms=$(( ($(date +%s%N) - start) / 1000000 ))\n",
    )


TOOK_RE = re.compile(r"took_ms=(\d+)")


def _writer_waits_for_reader(cijoe, name, option=""):
    """While the extents are held, a writer's open() waits and the extents keep the file's data;
    once they are released the writer goes ahead, and the file is mapped again after it."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/{name}"
    _fill(cijoe, path, "A", 4096)

    pid, byte, log = _start_hold(cijoe, dev_path, mountpoint, path, option)
    try:
        assert _raw(cijoe, dev_path, byte) == "AAAA", "held extent does not hold the file's data"

        # fsync, so that without the lease the write would reach the device within the second
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        time.sleep(1)

        assert "took_ms" not in (writer.read_text() if writer.exists() else ""), (
            "the writer was not held back by the lease"
        )
        assert _raw(cijoe, dev_path, byte) == "AAAA", "the held extent changed under the reader"

        cijoe.run(f"sudo kill -USR1 {pid}")
        took = int(_wait_for(writer, TOOK_RE, 10).group(1))
        assert took >= 900, f"writer took {took} ms, less than the 1 s the extents were held"
        assert int(_wait_for(log, RELEASED_RE, 10).group(1)) == 0, "held extents reported stale"

        m = _wait_for(log, REACQUIRED_RE, 30)
        assert _raw(cijoe, dev_path, int(m.group(1)), 1) == "B", "remapped extent lacks the write"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {path}")


def test_lease_writer_waits_for_reader(cijoe):
    _writer_waits_for_reader(cijoe, "xal_lease_fence")


def test_lease_acquired_on_exited_thread(cijoe):
    """The break reaches xal even when the thread that took the lease has exited."""

    _writer_waits_for_reader(cijoe, "xal_lease_thread", "--thread")


def test_lease_other_file_not_delayed(cijoe):
    """A writer waiting on one held file does not hold up the break of another file's lease,
    and truncate(2) on a path breaks a lease as open() does."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, idle = f"{mountpoint}/xal_lease_held", f"{mountpoint}/xal_lease_idle"
    _fill(cijoe, held, "A", 4096)
    _fill(cijoe, idle, "A", 4096)

    pid, _, _ = _start_hold(cijoe, dev_path, mountpoint, held, f"--idle {idle}")
    try:
        _timed(cijoe, "lease_held_writer", f"printf B | dd of={held} conv=notrunc status=none")
        time.sleep(0.5)

        # truncate(2), which waits for the lease; coreutils truncate opens with O_NONBLOCK
        truncater = _timed(
            cijoe,
            "lease_idle_truncate",
            f"python3 -c 'import os; os.truncate(\"{idle}\", 0)' && echo truncated",
        )
        took = int(_wait_for(truncater, TOOK_RE, 10).group(1))
        assert "truncated" in truncater.read_text(), truncater.read_text()
        assert _capture(cijoe, f"stat -c %s {idle}") == "0", "truncate did not take effect"
        assert took < 2000, f"truncate of a file without readers took {took} ms"
    finally:
        cijoe.run(f"sudo kill -USR1 {pid}")
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {held} {idle}")


def test_lease_refused_while_open_for_writing(cijoe):
    """A file held open for writing gets no lease, so its extents are not handed out."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_unsealed"
    _fill(cijoe, path, "A", 4096)

    fifo = _artifacts(cijoe) / "lease_unsealed.fifo"
    cijoe.run(f"rm -f {fifo}")
    cijoe.run(f"mkfifo {fifo}")
    holder = _background(
        cijoe, "lease_holder", f"exec 3>>{path}\necho opened\nread line < {fifo}\necho closed\n"
    )
    _wait_for(holder, re.compile("opened"), 10)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=-11" in out, f"lease granted on a file open for writing:\n{out}"

    cijoe.run(f"sh -c 'echo done > {fifo}'")
    _wait_for(holder, re.compile("closed"), 10)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, f"lease refused after the writer closed:\n{out}"

    cijoe.run(f"rm -f {path} {fifo}")


def test_lease_maps_dirty_data_after_writeback(cijoe):
    """Data still in the page cache is written back before mapping, so the extents have a disk
    address and hold the data."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_dirty"
    _fill(cijoe, path, "C", 65536, sync=False)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, out
    m = EXTENT_RE.search(out)
    assert m, f"no extent reported:\n{out}"
    byte, flag = int(m.group(1)), int(m.group(3), 16)

    assert not flag & FIEMAP_EXTENT_DELALLOC, f"delalloc extent handed out:\n{out}"
    assert byte, f"extent without a disk address:\n{out}"
    assert _raw(cijoe, dev_path, byte) == "CCCC", "extent does not hold the written data"

    cijoe.run(f"rm -f {path}")


def test_lease_leaves_out_unwritten(cijoe):
    """Unwritten extents read as zeros through the kernel, so they are left out like holes."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_unwritten"
    cijoe.run(f"fallocate -l 1M {path}")
    cijoe.run(f"sh -c \"printf '%4096s' | tr ' ' D | dd of={path} conv=notrunc,fsync status=none\"")

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, out
    m = EXTENT_RE.search(out)
    assert m and int(m.group(2)) == 4096, f"expected only the written 4 KiB:\n{out}"
    assert "EXTENT 1 " not in out, f"unwritten extent handed out:\n{out}"
    assert _raw(cijoe, dev_path, int(m.group(1))) == "DDDD", out

    cijoe.run(f"rm -f {path}")


def test_lease_refused_in_forked_child(cijoe):
    """A forked child cannot use the parent's leases, so it gets -ECHILD. Neither its close of the
    inherited descriptors at fork() nor xal_close() there drops the parent's lease, and
    xal_close() leaves alone the descriptors the child has opened since. xal_lease_stats() there
    does not wait for a lock held at fork()."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_fork"
    _fill(cijoe, path, "A", 4096)

    out = _capture(
        cijoe, f"sudo timeout -s KILL 20 xal_lease_hold {dev_path} {mountpoint} {path} --fork"
    )
    assert "CHILD_ACQUIRE err=-10" in out, out
    assert "CHILD_STATS err=0" in out, f"xal_lease_stats() failed in the child:\n{out}"
    assert "PARENT_LEASE=1" in out, f"the child's close released the parent's lease:\n{out}"
    assert "CHILD_FDS=64/64" in out, f"xal_close() in the child closed its descriptors:\n{out}"

    cijoe.run(f"rm -f {path}")


def test_lease_orphan_child_keeps_no_lease(cijoe):
    """A forked child does not keep the leases it inherits: after the parent exits without
    xal_close() while the child lives, a writer goes ahead at once instead of waiting
    lease-break-time for a lease nobody releases."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_orphan"
    _fill(cijoe, path, "A", 4096)

    cijoe.run("sudo sysctl -qw fs.lease-break-time=5")
    out = _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {path} --orphan")
    m = re.search(r"ORPHAN child=(\d+)", out)
    assert m, out
    try:
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        took = int(_wait_for(writer, TOOK_RE, 15).group(1))
        assert took < 1000, f"writer took {took} ms after the parent exited"
        err, _ = cijoe.run(f"sudo kill -0 {m.group(1)}")
        assert not err, "the child was gone by the time the writer finished"
    finally:
        cijoe.run(f"sudo kill -KILL {m.group(1)}")

    cijoe.run(f"rm -f {path}")


def test_lease_release_without_acquire(cijoe):
    """A release of a file that has no reader left is refused."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_release_twice"
    _fill(cijoe, path, "A", 4096)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, out
    assert "RELEASE err=0" in out, out
    assert f"RELEASE_AGAIN err=-{EINVAL}" in out, out

    cijoe.run(f"rm -f {path}")


def _owners(cijoe, mountpoint, byte, nbytes) -> set:
    """Owners GETFSMAP reports for the device byte range: inode_<ino>_data, FMR_OWN_FREE for free
    space, and so on."""

    out = _capture(cijoe, f"sudo xfs_io -c 'fsmap -d -m' {mountpoint}")
    lo, hi = byte // 512, (byte + nbytes) // 512 - 1
    owners = set()
    for line in out.splitlines():
        f = line.split(",")
        if len(f) > 5 and f[3].isdigit() and int(f[3]) <= hi and lo <= int(f[4]):
            owners.add(f[5])
    return owners


def test_lease_release_stale_after_dedupe(cijoe):
    """A dedupe into the held file through a read-only descriptor breaks no lease, but moves the
    file to other blocks and frees the old ones, so the release says the extents may no longer
    hold its data."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, twin = f"{mountpoint}/xal_lease_dedupe", f"{mountpoint}/xal_lease_twin"
    _fill(cijoe, held, "E", 65536)
    _fill(cijoe, twin, "E", 65536)

    pid, byte, log = _start_hold(cijoe, dev_path, mountpoint, held)
    try:
        out = _capture(cijoe, f"sudo xfs_io -r -c 'dedupe {twin} 0 0 65536' {held}")
        assert "deduped 65536/65536" in out, f"dedupe did not go through:\n{out}"
        owners = _owners(cijoe, mountpoint, byte, 65536)
        if any(o.startswith("inode_") for o in owners) or FMR_OWN_FREE in owners:
            assert FMR_OWN_FREE in owners, f"the held blocks were not freed: {owners}"

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
        assert err == -ESTALE, f"release returned {err} after the file was deduped"

        m = _wait_for(log, REACQUIRED_RE, 30)
        assert int(m.group(1)) != byte, "the file was not mapped again after the dedupe"
        assert _raw(cijoe, dev_path, int(m.group(1))) == "EEEE", "remapped extent lacks the data"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {held} {twin}")


def test_lease_release_misses_dedupe_with_no_recheck(cijoe):
    """With lease_no_recheck the release does not map the file again, so it does not see the
    dedupe that test_lease_release_stale_after_dedupe finds, and the next acquire hands out the
    old extents."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, twin = f"{mountpoint}/xal_lease_dedupe_nr", f"{mountpoint}/xal_lease_twin_nr"
    _fill(cijoe, held, "E", 65536)
    _fill(cijoe, twin, "E", 65536)

    pid, byte, log = _start_hold(cijoe, dev_path, mountpoint, held, "--no-recheck")
    try:
        out = _capture(cijoe, f"sudo xfs_io -r -c 'dedupe {twin} 0 0 65536' {held}")
        assert "deduped 65536/65536" in out, f"dedupe did not go through:\n{out}"
        owners = _owners(cijoe, mountpoint, byte, 65536)
        if any(o.startswith("inode_") for o in owners) or FMR_OWN_FREE in owners:
            assert FMR_OWN_FREE in owners, f"the held blocks were not freed: {owners}"

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
        assert err == 0, f"release returned {err} with lease_no_recheck"
        m = _wait_for(log, REACQUIRED_RE, 30)
        assert int(m.group(1)) == byte, "the next acquire did not hand out the old extents"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {held} {twin}")


def test_lease_release_stale_after_break_time(cijoe):
    """Past lease-break-time the kernel lets a writer through while the extents are held, so the
    release says they may no longer hold the file's data."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_timeout"
    _fill(cijoe, path, "A", 4096)

    cijoe.run("sudo sysctl -qw fs.lease-break-time=1")
    pid, byte, log = _start_hold(cijoe, dev_path, mountpoint, path)
    try:
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        took = int(_wait_for(writer, TOOK_RE, 10).group(1))
        assert took < 5000, f"writer took {took} ms with a lease-break-time of 1 s"
        assert _raw(cijoe, dev_path, byte, 1) == "B", "the writer did not reach the held extent"

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
        assert err == -ESTALE, f"release returned {err} after the writer was let through"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {path}")


def test_lease_release_stale_when_break_seen_late(cijoe):
    """A break that reaches xal only after the kernel let the writer through is still reported:
    the kernel no longer lists the lease, although lease-break-time has not run out since xal saw
    the break. The holder is stopped meanwhile, so the break waits in its signal queue; the
    release comes well within the 3 s xal then counts from the break."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_late"
    _fill(cijoe, path, "A", 4096)

    cijoe.run("sudo sysctl -qw fs.lease-break-time=3")
    pid, _, log = _start_hold(cijoe, dev_path, mountpoint, path)
    try:
        cijoe.run(f"sudo kill -STOP {pid}")
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        _wait_for(writer, TOOK_RE, 10)
        cijoe.run(f"sudo kill -CONT {pid}")
        time.sleep(0.2)

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
        assert err == -ESTALE, f"release returned {err} after a break seen late"
    finally:
        cijoe.run(f"sudo kill -CONT {pid}")
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {path}")


def test_lease_recheck_ignores_shared_split(cijoe):
    """Cloning part of the held file into another file breaks no lease and moves no data, but
    XFS then reports the held file's mapping split where it is shared, which the recheck must not
    take for a change."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, other = f"{mountpoint}/xal_lease_split", f"{mountpoint}/xal_lease_split_clone"
    _fill(cijoe, held, "E", 65536)

    pid, _, log = _start_hold(cijoe, dev_path, mountpoint, held)
    try:
        out = _capture(cijoe, f"sudo xfs_io -f -c 'reflink {held} 0 0 4096' {other}")
        assert "linked 4096/4096" in out, f"reflink did not go through:\n{out}"
        out = _capture(cijoe, f"sudo xfs_io -r -c 'fiemap' {held}")
        assert len(re.findall(r"^\s*\d+:", out, re.M)) >= 2, f"FIEMAP did not split:\n{out}"

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
        assert err == 0, f"release returned {err} although no data moved"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"sudo rm -f {held} {other}")


def _recall(cijoe, name):
    """Hold a file with --poll and open it for writing; return (left_ms, took_ms) once the
    reader has seen the recall, released, and the writer is done."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/{name}"
    _fill(cijoe, path, "A", 4096)

    pid, _, log = _start_hold(cijoe, dev_path, mountpoint, path, "--poll")
    try:
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        left = int(_wait_for(log, RECALLED_RE, 10).group(1))
        took = int(_wait_for(writer, TOOK_RE, 10).group(1))
        assert int(_wait_for(log, RELEASED_RE, 10).group(1)) == 0, "release after recall stale"

        m = _wait_for(log, REACQUIRED_RE, 30)
        assert _raw(cijoe, dev_path, int(m.group(1)), 1) == "B", "remapped extent lacks the write"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {path}")
    return left, took


def test_lease_reader_sees_recall(cijoe):
    """A reader polling xal_extents_recalled() learns that a writer waits and how long is left
    of lease-break-time, and by releasing lets the writer go ahead at once."""

    breaktime = int(_capture(cijoe, "cat /proc/sys/fs/lease-break-time"))
    if breaktime <= 0:
        pytest.skip("lease-break-time is 0, see test_lease_recall_without_break_time")

    left, took = _recall(cijoe, "xal_lease_recall")
    assert (breaktime - 2) * 1000 < left < breaktime * 1000, f"{left} ms left of {breaktime} s"
    assert took < 1000, f"writer took {took} ms although the reader released on the recall"


def test_lease_recall_without_break_time(cijoe):
    """With a lease-break-time of 0 the kernel waits for readers however long they take, which
    the recall reports as no time limit."""

    cijoe.run("sudo sysctl -qw fs.lease-break-time=0")
    left, _ = _recall(cijoe, "xal_lease_recall_nolimit")
    assert left == -1, f"{left} ms left reported with a lease-break-time of 0"


def test_lease_recall_after_stale_release(cijoe):
    """Once a release has found the extents stale, the other readers are told to release now:
    left_ms is 0, which only the stale flag gives. Their release says the extents are stale too."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, twin = f"{mountpoint}/xal_lease_stale_recall", f"{mountpoint}/xal_lease_stale_twin"
    _fill(cijoe, held, "E", 65536)
    _fill(cijoe, twin, "E", 65536)

    pid, _, log = _start_hold(cijoe, dev_path, mountpoint, held, "--twice")
    try:
        out = _capture(cijoe, f"sudo xfs_io -r -c 'dedupe {twin} 0 0 65536' {held}")
        assert "deduped 65536/65536" in out, f"dedupe did not go through:\n{out}"

        cijoe.run(f"sudo kill -USR1 {pid}")
        assert int(_wait_for(log, RELEASED_RE, 10).group(1)) == -ESTALE, log.read_text()
        assert int(_wait_for(log, RECALLED_RE, 10).group(1)) == 0, log.read_text()

        cijoe.run(f"sudo kill -USR1 {pid}")
        m = _wait_for(log, re.compile(r"RELEASED_OTHER err=(-?\d+)"), 10)
        assert int(m.group(1)) == -ESTALE, log.read_text()
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {held} {twin}")


def _sweep(cijoe, name, nfiles, options="", nofile=None):
    """Make nfiles files under a directory, acquire and release each; return the SWEEP counts."""

    dev_path, mountpoint = _require_fixture(cijoe)
    directory = f"{mountpoint}/{name}"
    cijoe.run(f"mkdir -p {directory}")
    cijoe.run(f"sh -c 'for i in $(seq {nfiles}); do printf A > {directory}/f$i; done; sync'")
    limit = f"ulimit -n {nofile} && " if nofile else ""
    out = _capture(
        cijoe,
        f"sudo sh -c '{limit}xal_lease_hold {options} --sweep {dev_path} {mountpoint} {directory}'",
    )
    cijoe.run(f"rm -rf {directory}")
    m = SWEEP_RE.search(out)
    assert m, f"no sweep result:\n{out}"
    return dict(zip(("files", "failed", "err", "leases", "evicted"), map(int, m.groups())))


def test_lease_drops_idle_past_lease_max(cijoe):
    """Past lease_max, xal drops the leases of files without readers instead of holding more."""

    r = _sweep(cijoe, "xal_lease_sweep", 10, "--max 4")
    assert r["files"] == 10 and r["failed"] == 0, r
    assert r["leases"] <= 4, f"more leases held than lease_max: {r}"
    assert r["evicted"] == 6, r


def test_lease_drops_idle_at_nofile_limit(cijoe):
    """By default xal keeps leases on at most half the soft RLIMIT_NOFILE, so mapping more files
    than there are descriptors does not fail."""

    r = _sweep(cijoe, "xal_lease_nofile", 200, nofile=64)
    assert r["files"] == 200, r
    assert r["failed"] == 0, f"mapping failed past the descriptor limit: {r}"
    assert r["leases"] <= 32, f"more leases held than half of RLIMIT_NOFILE: {r}"


def test_lease_drops_idle_when_out_of_descriptors(cijoe):
    """With lease_max above RLIMIT_NOFILE, xal drops a lease when open() runs out of descriptors
    instead of failing the mapping."""

    r = _sweep(cijoe, "xal_lease_emfile", 200, "--max 1000", nofile=64)
    assert r["files"] == 200, r
    assert r["failed"] == 0, f"mapping failed when out of descriptors: {r}"
    assert r["evicted"] > 0, r


def _release_with_no_descriptor_free(cijoe, name, options, breaktime_at_open):
    """Hold a file under ulimit -n 64 with options that leave no descriptor free, let a writer
    through with a lease-break-time of 1 s, and return what the release said."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/{name}"
    fill = f"{mountpoint}/{name}_fill"
    _fill(cijoe, path, "A", 4096)
    cijoe.run(f"mkdir -p {fill}")
    cijoe.run(f"sh -c 'for i in $(seq 100); do printf A > {fill}/f$i; done; sync'")

    cijoe.run(f"sudo sysctl -qw fs.lease-break-time={breaktime_at_open}")
    log = _artifacts(cijoe) / "lease_hold.log"
    cijoe.run(f"sudo rm -f {log}")
    cijoe.run(
        f"sudo sh -c 'ulimit -n 64 && nohup xal_lease_hold {options.format(fill=fill)} "
        f"{dev_path} {mountpoint} {path} > {log} 2>&1 & echo backgrounded'"
    )
    pid = int(_wait_for(log, READY_RE, 30).group(1))
    try:
        cijoe.run("sudo sysctl -qw fs.lease-break-time=1")
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        _wait_for(writer, TOOK_RE, 10)

        cijoe.run(f"sudo kill -USR1 {pid}")
        err = int(_wait_for(log, RELEASED_RE, 10).group(1))
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -rf {path} {fill}")
    return err


def test_lease_release_stale_with_no_descriptor_free(cijoe):
    """With every descriptor taken by idle leases, as lease_max above RLIMIT_NOFILE allows, the
    release drops one to read fdinfo. The handle opens at lease-break-time 45, which is set to 1
    after LEASE_READY; xal cannot read it at the break, so it still goes by 45 s, and only fdinfo
    can tell."""

    err = _release_with_no_descriptor_free(cijoe, "xal_lease_nofd", "--max 1000 --idle {fill}", 45)
    assert err == -ESTALE, f"release returned {err} with no descriptor free"


def test_lease_release_stale_with_no_descriptor_to_drop(cijoe):
    """With every descriptor taken and no idle lease to drop, the release cannot read fdinfo and
    goes by the lease-break-time read when the handle opened, not by 45 s."""

    err = _release_with_no_descriptor_free(cijoe, "xal_lease_nofd_none", "--exhaust", 1)
    assert err == -ESTALE, f"release returned {err} with no descriptor to drop"


def test_lease_rejects_shm(cijoe):
    """Leases pin nothing for other processes, so a shared index is refused."""

    dev_path, mountpoint = _require_fixture(cijoe)

    out = _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {mountpoint} --shm")
    assert f"OPEN err=-{ENOTSUP}" in out, out
