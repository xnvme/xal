#!/usr/bin/env python3
"""
Populate a mountpoint with files and dictories allocated with different on-disk-format
======================================================================================

The intent for this script is to create a collection of files and folders which will
trigger XFS to utilize the different formats: FMT_LOCAL, FMT_EXTENTS, and FMT_BTREE for
the storage of directory entries and file-content.
"""

import logging as log
from argparse import ArgumentParser, Namespace
from pathlib import Path
from cijoe.core.command import Cijoe
import os
import re
import json


def str2bool(value: str) -> bool:
    """Parse a cijoe '--flag <value>' string as a boolean.

    cijoe always renders a workflow 'with' entry as '--flag <value>', so a boolean flag must take
    a value. A plain truthiness check would treat 'false'/'0' (a non-empty string) as True; this
    parses the intended boolean instead.
    """

    return str(value).strip().lower() in ("1", "true", "yes", "on")


COLUMNS = ["startoffset", "endoffset", "startblock", "endblock"]


def create_directory_with_urandom_content(
    args: Namespace, cijoe: Cijoe, dir: Path, count: int, size: str
) -> int:
    """
    In 'dir' create 'count' files with 'size' amount of content from '/dev/urandom'
    """

    for cmd in [
        f"mkdir -p {dir}",
        (
            f'i=0; while [ "$i" -le ${count} ]; do '
            f"dd if=/dev/urandom of={dir}/file-rand-{size}-$i-{count}.bin status=none "
            f"bs={size} count=1"
            "; i=$((i + 1)); done"
        ),
    ]:
        err, state = cijoe.run(cmd)
        if err:
            return err

    return 0


def provoke_odf_dir_fmt_local(args: Namespace, cijoe: Cijoe) -> int:
    """
    This creates a directory with a few small files in it, which should be able to store
    the entire contents in locally / inside the inode, that is, use FMT_LOCAL.

    TODO:

    * trigger shortform + longform
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "should-be-dir-fmt_local", 5, "4K"
    )


def provoke_odf_dir_fmt_extents_few(args: Namespace, cijoe: Cijoe) -> int:
    """
    Create a directory with more files than what can fit in FMT_LOCAL, triggering the
    use of FMT_EXTENTS. This should still not provoke the use of FMT_BTREE, since that
    would require more fragmentation.

    This be possible with a single extents
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "should-be-dir-fmt_extents-few", 100, "4K"
    )


def provoke_odf_dir_fmt_extents_more(args: Namespace, cijoe: Cijoe) -> int:
    """
    Create a directory with more files than what can fit in FMT_LOCAL, triggering the
    use of FMT_EXTENTS. This should still not provoke the use of FMT_BTREE, since that
    would require more fragmentation.

    This spills into multiple extents.
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "should-be-dir-fmt_extents-more", 500, "4K"
    )


def provoke_odf_dir_fmt_btree(args: Namespace, cijoe: Cijoe) -> int:
    """
    Create a directory containing many files requiring the use of FMT_BTREE
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "should-be-dir-fmt_btree", 5000, "4K"
    )


def provoke_odf_file_fmt_local(args: Namespace, cijoe: Cijoe) -> int:
    """Create empty files, these should be represented as FMT_LOCAL?"""

    prefix = args.mountpoint / "should-be-file-fmt_local"

    total = 5
    for cur in range(1, total + 1):
        filepath = prefix / f"empty-{cur}-{total}.file"
        for cmd in [f"mkdir -p {prefix}", f"touch {filepath}"]:
            err, _ = cijoe.run(cmd)
            if err:
                return err

    return 0


def provoke_odf_file_fmt_extents(args: Namespace, cijoe: Cijoe) -> int:
    """Create small files represented as FMT_EXTENTS"""

    path = args.mountpoint / "should-be-file-fmt_extents"
    for size in [1, 4, 10]:
        if err := create_directory_with_urandom_content(
            args, cijoe, path, 1, f"{size}"
        ):
            return err

    err, _ = cijoe.run(f"touch {path / 'empty.file'}")

    return err


def provoke_odf_file_fmt_btree(args: Namespace, cijoe: Cijoe) -> int:
    """
    Creates a file to trigger B+tree on-disk allocation for file content storage.

    This is typically needed due to fragmentation and can be provoked by introducing
    "holes" during file allocation. Specifically, here 600 calls to `fallocate` allocate
    4 KB segments at non-contiguous 4 KB offsets. The resulting small file forces B+tree
    usage, as each gap requires a new extent.
    """

    prefix = args.mountpoint / "should-be-file-fmt_btree"

    for nextents in [600, 6000]:
        filepath = prefix / f"fragmented-nextents-{nextents}"

        commands = [
            f"mkdir -p {prefix} && truncate -s 0 {filepath}",
            (
                f'i=0; while [ "$i" -le ${nextents} ]; do '
                f"fallocate -o $((i * 8192)) -l 4096 {filepath}"
                "; i=$((i + 1)); done"
            ),
        ]
        for cmd in commands:
            err, _ = cijoe.run(cmd)
            if err:
                return err

    return 0


def provoke_odf_iabt_lvl0(args: Namespace, cijoe: Cijoe) -> int:
    """
    This is the base to use when trying to provoke different layout for the
    inode-allocation-btree (IAB3). With this, the root node is level 0 and records inline.
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "intent_iab3_lvl0", 10, "4K"
    )


def provoke_odf_iabt_lvl1(args: Namespace, cijoe: Cijoe) -> int:
    """
    This is the base to use when trying to provoke different layout for the
    inode-allocation-btree (IAB3). With this, the root node is level 1 and records be
    node pointers.
    """

    return create_directory_with_urandom_content(
        args, cijoe, args.mountpoint / "intent_iab3_lvl1", 20000, "4K"
    )


def populate(args, cijoe) -> int:
    """Explicitly create different files and folders to provoke all ODF cases"""

    if err := provoke_odf_iabt_lvl0(args, cijoe):
        return err

    if err := provoke_odf_iabt_lvl1(args, cijoe):
        return err

    if err := provoke_odf_dir_fmt_local(args, cijoe):
        return err

    if err := provoke_odf_dir_fmt_extents_few(args, cijoe):
        return err

    if err := provoke_odf_dir_fmt_extents_more(args, cijoe):
        return err

    if err := provoke_odf_dir_fmt_btree(args, cijoe):
        return err

    if err := provoke_odf_file_fmt_local(args, cijoe):
        return err

    if err := provoke_odf_file_fmt_btree(args, cijoe):
        return err

    return 0


def umount(args: Namespace, cijoe: Cijoe) -> int:
    """Unmount the file-system at args.mountpoint"""

    err, state = cijoe.run(f"sudo umount {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed un-mounting")
        return err

    return 0


def mount(args: Namespace, cijoe: Cijoe) -> int:
    """
    Does the following:

    * Creates directory at 'args.mountpoint' to use as mountpoint; can exist
    * Mounts 'args.mountpoint' to it
    * Changes ownership of it any anything inside to $USER

    Returns error in case any of the above fails, or if mountpoint is already mounted.
    """

    # Release any prior mount at this mountpoint (e.g. a leftover from a previous run that used
    # keep_mounted) so the configured dev_path is what actually gets mounted here -- not a stale
    # device. Previously an "already mounted" mountpoint returned success WITHOUT mounting, which
    # silently ran the tests against whatever device happened to be mounted.
    cijoe.run(f"sudo umount {args.mountpoint} || true")

    err, state = cijoe.run(f"sudo mkdir -p {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed creating")
        return err

    err, state = cijoe.run(f"sudo mount {args.dev_path} {args.mountpoint}")
    if err:
        log.error(f"mountpoint({args.mountpoint}); failed mounting")
        return err

    err, state = cijoe.run(f"sudo chown -R $USER:$USER {args.mountpoint}")
    if err:
        log.error(f"chown; failed")
        return err

    return 0


def produce_bmap(args: Namespace, cijoe: Cijoe):

    def get_extents(path: Path):
        """The given path is expected to be absolute"""

        regex = (
            r"^(?P<ident>\d+):\s+"
            r"\[(?P<startoffset>\d+)\.\.(?P<endoffset>\d+)\]:\s+"
            r"(?P<startblock>\d+)\.\.(?P<endblock>\d+)$"
        )

        cmd = f"xfs_bmap {path}"
        err, state = cijoe.run(cmd)
        if err:
            print("Failed running: %s" % " ".join(cmd))
            return err

        extents = []
        for line in state.output().splitlines():
            match = re.match(regex, line.strip())
            if match:
                extent = {key: int(val) for key, val in match.groupdict().items()}
                extents.append([extent.get(col) for col in COLUMNS])

        return extents

    result = {}

    for root, _, files in os.walk(args.mountpoint):
        for name in files:
            path = Path(root) / name

            result[str(path)] = (os.stat(path).st_ino, get_extents(path))

    path_bmap = Path(cijoe.getconf("xal.artifacts.path")) / "bmap.json"
    path_bmap.parent.mkdir(exist_ok=True, parents=True)
    path_bmap.write_text(json.dumps(result))

    return 0


def produce_index(args: Namespace, cijoe: Cijoe):
    """Create a recursive index of all the files and directories in args.mountpoint"""

    path_index = Path(cijoe.getconf("xal.artifacts.path")) / "find.output"
    path_index.parent.mkdir(exist_ok=True, parents=True)

    err, state = cijoe.run(f"find {args.mountpoint} | sort > {path_index}")
    if err:
        return err

    return 0


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--dev-path",
        type=Path,
        help="Path to the block device",
    )
    parser.add_argument(
        "--mountpoint",
        type=Path,
        help="Path to mountpoint",
    )
    parser.add_argument(
        "--produce_index",
        type=Path,
        help="Produce an recursive index of all the files at --mountpoint",
    )
    parser.add_argument(
        "--produce_bmap",
        type=Path,
        help="Produce a bmap.json file containing all extents for files",
    )
    parser.add_argument(
        "--keep_mounted",
        type=str2bool,
        default=False,
        help="Leave the filesystem mounted after populating (for tests needing a live mount)",
    )
    parser.add_argument(
        "--skip_populate",
        type=str2bool,
        default=False,
        help="Only mount (skip file population); for tests that provision their own files",
    )


def main(args: Namespace, cijoe: Cijoe) -> int:
    """Entry-point of the cijoe-script"""

    if err := mount(args, cijoe):
        return err

    # Reflink tests provision their own small file set; skipping the ~25k-file population keeps
    # the whole-tree snapshot fast and bounded (and light on a RAM-backed zram device).
    if not getattr(args, "skip_populate", None):
        if err := populate(args, cijoe):
            return err

    if args.produce_index:
        if err := produce_index(args, cijoe):
            return err

    if args.produce_bmap:
        if err := produce_bmap(args, cijoe):
            return err

    # Tests that use the FIEMAP backend (e.g. reflink-snapshot) need the mount to persist;
    # the XFS-backend tests read the raw device and prefer it unmounted.
    if not getattr(args, "keep_mounted", None):
        if err := umount(args, cijoe):
            return err

    return 0


if __name__ == "__main__":
    main()
