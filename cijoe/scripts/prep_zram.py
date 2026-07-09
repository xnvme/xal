#!/usr/bin/env python3
"""
Virtual block-device using zram
===============================

Crete a single block device of a given size in GB.

"""

import os
import sys
import subprocess
import errno
import logging as log
from pathlib import Path
from argparse import ArgumentParser, Namespace
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    """Optional function for defining command-line arguments for this script"""

    parser.add_argument(
        "--size-in-gb",
        type=int,
        default=1,
        help="Size of the zram device, in bytes",
    )
    parser.add_argument(
        "--dev-path",
        type=str,
        default=Path("/dev/zram0"),
        help="Path to the zram device",
    )


def main(args: Namespace, cijoe: Cijoe):
    """Entry-point of the cijoe-script"""

    size_bytes = int(args.size_in_gb << 30)
    dev_path = cijoe.getconf("xal.dev_path", args.dev_path)

    if "zram" not in dev_path:
        log.info(f"Substr 'zram' not in dev_path({dev_path}); skipping")
        return 0

    # Release any stale binding from a prior run (a left-behind mount makes --reset fail EBUSY).
    cijoe.run(f'sudo umount {dev_path} || echo "Above is OK."')
    cijoe.run(f'sudo swapoff {dev_path} || echo "Above error is OK."')
    cijoe.run(f'sudo zramctl --reset {dev_path} || echo "Above is OK."')
    cijoe.run(f'sudo modprobe -r zram || echo "Above is OK."')

    err, state = cijoe.run(f"[[ -f {dev_path} ]]")

    err, state = cijoe.run(f"sudo modprobe zram num_devices=1")
    if err:
        log.error("Failed loading zram module.")
        return err

    err, state = cijoe.run(f"sudo zramctl --size {size_bytes} {dev_path}")
    if err:
        log.error("Failing resizing device")
        return err

    err, state = cijoe.run(f"sudo chown $USER:$USER {dev_path}")
    if err:
        log.error("...")
        return err

    return 0
