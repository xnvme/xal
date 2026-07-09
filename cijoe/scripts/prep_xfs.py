#!/usr/bin/env python3
"""
Format block device with XFS
============================

Format the given block device with XFS
"""

import logging as log
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe
from pathlib import Path


def str2bool(value: str) -> bool:
    """Parse a cijoe '--flag <value>' string as a boolean.

    cijoe always renders a workflow 'with' entry as '--flag <value>', so a boolean flag must take
    a value. A plain truthiness check would treat 'false'/'0' (a non-empty string) as True; this
    parses the intended boolean instead.
    """

    return str(value).strip().lower() in ("1", "true", "yes", "on")


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

    err, state = cijoe.run(f"mountpoint {args.mountpoint}")
    if not err:
        log.error(f"mountpoint({args.mountpoint}); already mounted")
        return err

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


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--dev-path",
        type=str,
        help="Path to the block device",
    )
    parser.add_argument(
        "--mountpoint",
        type=Path,
        help="Path to mountpoint",
    )
    parser.add_argument(
        "--reflink",
        type=str2bool,
        default=False,
        help="Format with reflink=1 (required for the reflink-snapshot tests)",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    # Safety net: never mkfs a mounted device -- that would destroy live data. The loop/zram preps
    # unmount their device beforehand; a real device (e.g. nvme) must be a dedicated, unmounted
    # target. This also protects against a config that points dev_path at a disk holding data.
    err, _ = cijoe.run(f"findmnt --source {args.dev_path} > /dev/null 2>&1")
    if not err:
        log.error(
            f"Refusing to mkfs {args.dev_path}: it is mounted. Unmount it, or point the config "
            f"at a dedicated spare device/namespace."
        )
        return 1

    # reflink=1 is the mkfs.xfs default on recent xfsprogs, but request it explicitly so the
    # reflink-snapshot tests do not depend on the tool's default.
    reflink_opt = "-m reflink=1 " if getattr(args, "reflink", False) else ""

    err, state = cijoe.run(
        f"sudo mkfs.xfs {reflink_opt}-b size=4096 -n size=8192 -f {args.dev_path}"
    )
    if err:
        log.error("Failed creating filesystem")
        return err

    return 0


if __name__ == "__main__":
    main()
