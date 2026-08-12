#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for mount topology and privileged-helper shutdown safety."""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gui"))

from core import devices
import privileged_helper


def _link(link: Path, target: Path) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target, target_is_directory=True)


def test_block_topology() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-topology-") as directory:
        root = Path(directory)
        nodes = root / "nodes"
        sys_block = root / "sys-dev-block"
        disk = nodes / "block" / "sda"
        partition = disk / "sda1"
        sibling = disk / "sda2"
        mapper = nodes / "virtual" / "block" / "dm-0"
        partition.mkdir(parents=True)
        sibling.mkdir(parents=True)
        mapper.mkdir(parents=True)
        sys_block.mkdir()
        _link(sys_block / "8:0", disk)
        _link(sys_block / "8:1", partition)
        _link(sys_block / "8:2", sibling)
        _link(sys_block / "253:0", mapper)
        _link(disk / "holders" / "dm-0", mapper)
        _link(mapper / "slaves" / "sda", disk)

        old_root = devices._SYS_DEV_BLOCK
        devices._SYS_DEV_BLOCK = sys_block
        try:
            # Whole-disk mutation overlaps both child partitions.
            assert devices._related_block_ids("8:0") == {
                "8:0", "8:1", "8:2", "253:0"
            }
            # Partition mutation overlaps its parent container and mappings,
            # but NOT the disjoint sibling partition.
            assert devices._related_block_ids("8:1") == {"8:0", "8:1", "253:0"}
            # The whole-disk mapper still overlaps every partition of its slave.
            assert devices._related_block_ids("253:0") == {
                "8:0", "8:1", "8:2", "253:0"
            }
        finally:
            devices._SYS_DEV_BLOCK = old_root


def test_regular_image_mount_source() -> None:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-mountinfo-") as directory:
        root = Path(directory)
        image = root / "volume.img"
        image.write_bytes(b"\0" * 4096)
        mountinfo = root / "mountinfo"
        mountinfo.write_text(
            f"36 25 7:0 / /mnt rw,relatime - ext4 {image} rw\n",
            encoding="utf-8",
        )
        old_mountinfo = devices._MOUNTINFO
        old_sysfs = devices._SYS_DEV_BLOCK
        devices._MOUNTINFO = mountinfo
        devices._SYS_DEV_BLOCK = root / "empty-sysfs"
        try:
            assert devices.is_mounted(str(image))
            try:
                devices.require_unmounted(str(image))
            except RuntimeError as exc:
                assert "mounted" in str(exc)
            else:
                raise AssertionError("mounted regular image was accepted for mutation")
        finally:
            devices._MOUNTINFO = old_mountinfo
            devices._SYS_DEV_BLOCK = old_sysfs


def test_helper_waits_for_writer() -> None:
    events: list[object] = []

    class FakeProcess:
        pid = 4321

        @staticmethod
        def poll() -> None:
            return None

    class FakeThread:
        def __init__(self) -> None:
            self.alive = True

        def is_alive(self) -> bool:
            return self.alive

        def join(self, timeout: float | None = None) -> None:
            events.append(("join", timeout))
            self.alive = False

    process = FakeProcess()
    worker = FakeThread()
    old_process = privileged_helper._active_process
    old_thread = privileged_helper._active_thread
    old_getpgid = os.getpgid
    old_killpg = os.killpg
    privileged_helper._active_process = process
    privileged_helper._active_thread = worker
    os.getpgid = lambda pid: pid
    os.killpg = lambda pgid, sig: events.append(("signal", pgid, sig))
    try:
        privileged_helper.stop_active_and_wait()
    finally:
        privileged_helper._active_process = old_process
        privileged_helper._active_thread = old_thread
        os.getpgid = old_getpgid
        os.killpg = old_killpg
    assert events[0][0] == "signal"
    assert events[1] == ("join", 0.1)


if __name__ == "__main__":
    test_block_topology()
    test_regular_image_mount_source()
    test_helper_waits_for_writer()
    print("mount-topology and privileged-helper safety tests passed")
