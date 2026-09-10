# xal-server

Builds an xal index for each configured device and publishes it over POSIX shared memory, so
several processes can query one index without each scanning the filesystem.

There is no socket and no handshake: the shared memory names in the configuration are the entire
interface between the server and its readers.

## Configuration

TOML, passed with `--config`. The install writes a template to
`$prefix/etc/xal/xal-server.conf.example` and nothing else -- `make` re-runs `meson install` on
every build, so writing `xal-server.conf` itself would discard your edits each time. Copy it once:

```bash
sudo cp /usr/local/etc/xal/xal-server.conf.example /usr/local/etc/xal/xal-server.conf
```

The template ships `devices = []`, which is rejected rather than treated as "nothing to do", so it
is a starting point and not a runnable config. The systemd unit and the commands below all name
`xal-server.conf`, so without that copy a fresh install has a unit pointing at a missing file.

```toml
# CRITICAL = 0, WARNING = 1, NOTICE = 2, INFO = 3, DEBUG = 4
log_level = 2

devices = [
  { uri = "/dev/nvme0n1", shm_name = "/xal_dev0" },
  { uri = "/dev/nvme0n2", shm_name = "/xal_dev1", mountpoint = "/mnt/data", subtree = "/mnt/data/videos" },
]

[xal]
# none = 0, dirty detection = 1, extent update = 2, reflink snapshot = 3
watchmode = 2
```

| Key | Required | Meaning |
|-----|----------|---------|
| `log_level` | no | syslog verbosity; defaults to NOTICE |
| `devices[].uri` | yes | block device to index; must be mounted |
| `devices[].shm_name` | yes | name readers pass to `xal_from_shm()` |
| `devices[].mountpoint` | no | looked up in `/proc/mounts` when omitted |
| `devices[].subtree` | no | absolute path at or under the mountpoint to restrict the index to |
| `xal.watchmode` | no | see `enum xal_watchmode`; defaults to none |

`shm_name` is spelled out rather than derived from the position in `devices`, because readers
depend on it: a name that shifted when the list was reordered would silently point a reader at a
different device. Two entries sharing a name is refused.

The server uses the FIEMAP backend, which needs the filesystem mounted. It never opens an xNVMe
device, so `xal_extent_in_lba()` does not work on an index it published -- use
`xal_extent_in_bytes()`.

## Running

```bash
sudo xal-server --config /usr/local/etc/xal/xal-server.conf
```

Or, where the systemd unit was installed (`systemctl cat xal-server` to check -- it is only
installed when systemd's pkg-config was present at configure time):

```bash
sudo systemctl daemon-reload
sudo systemctl start xal-server
journalctl -u xal-server -f
```

A `systemctl stop` issued while a device is still indexing waits for that index to finish, so a
large tree can exceed the unit's default `TimeoutStopSec` and be SIGKILLed -- which skips the
shared memory unlink. Raise it in `xal-server.service.in` if your indexes are long.

The server logs to syslog under the identity `xal-server`, exits on SIGTERM/SIGINT/SIGHUP, and
closes each index on the way out -- which unlinks the shared memory. Readers must be done before
then.

## Reading an index

A reader attaches with `xal_from_shm()` and gets a read-only handle supporting the full query API.
Everything else -- superblock, backend, mountpoint, root inode -- comes out of the shared state
region, so nothing has to be passed alongside the name.

```c
struct xal *xal;
struct xal_extents *extents;
int err;

err = xal_from_shm("/xal_dev0", &xal);
if (err) {
	return err;
}

err = xal_get_extents(xal, "/videos/clip.mp4", &extents);

xal_close(xal); // unmaps; does not unlink, the server owns the regions
```

`xal_open()` with `opts.shm_name` set reaches the same place: finding a published index under the
name, it attaches as a secondary instead of opening the device. `xal_get_procrole()` reports which
of the two happened.

Three errors from an attach mean "not yet", not failure, and a reader starting alongside the server
should retry on them:

| Error | Meaning |
| ----- | ------- |
| `-EAGAIN` | the region exists but the server is still setting it up |
| `-ESTALE` | the region is set up, but the first `xal_index()` has not finished |
| `-ENOENT` | no index is published under that name |

`-EPROTO` is the opposite: the server published a state region this reader cannot read, because the
two were built against different xal versions. Retrying never clears it; rebuild both sides.

## Staying current

With a watch mode other than `none`, the server watches each filesystem and re-indexes when it
changes, rewriting the pools in place under a sequence lock. Attached readers see the update
without reattaching, but a walk that raced the rewrite fails with `-ESTALE`; check
`xal_is_dirty()`, or retry.

`XAL_WATCHMODE_REFLINK_SNAPSHOT` is the exception: it pins extents with reflink clones at index
time rather than watching, so the published index does not change for the life of the server.

If re-indexing fails, the server logs `the index is stale, restart required` at CRITICAL and the
index stays dirty. It does not recover on its own.
