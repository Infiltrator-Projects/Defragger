#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Integration checks for the authoritative native C XFS plugin."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from filesystems.xfs.plugin import BACKEND


def _native_worker() -> Path:
    override = os.environ.get("LINUX_DEFRAGGER_XFS_WORKER")
    if override:
        return Path(override)
    candidate = ROOT / "build" / "linux-defragger-xfs-worker"
    if candidate.is_file():
        return candidate
    candidate = ROOT / "build82" / "linux-defragger-xfs-worker"
    if candidate.is_file():
        return candidate
    raise RuntimeError("native XFS worker is not built")


def _minimal_xfs(path: Path) -> None:
    """Create the smallest scanner fixture needed to exercise the C XFS reader.

    This is a binary test fixture, not a second filesystem implementation.  It
    contains one allocation group, an empty inode B+tree and one free-space
    record so identification, geometry and map translation can be validated.
    """

    block_size = 4096
    sector_size = 512
    dblocks = 256
    image = bytearray(dblocks * block_size)

    sb = memoryview(image)[:512]
    sb[:4] = b"XFSB"
    struct.pack_into(">I", sb, 4, block_size)
    struct.pack_into(">Q", sb, 8, dblocks)
    struct.pack_into(">Q", sb, 16, 0)
    sb[32:48] = bytes.fromhex("00112233445566778899aabbccddeeff")
    struct.pack_into(">Q", sb, 48, 8)  # internal log start
    struct.pack_into(">I", sb, 84, dblocks)
    struct.pack_into(">I", sb, 88, 1)
    struct.pack_into(">I", sb, 96, 8)
    struct.pack_into(">H", sb, 100, 5)
    struct.pack_into(">H", sb, 102, sector_size)
    struct.pack_into(">H", sb, 104, 512)
    struct.pack_into(">H", sb, 106, 8)
    sb[120:125] = bytes((12, 9, 9, 3, 8))
    struct.pack_into(">Q", sb, 128, 0)
    struct.pack_into(">Q", sb, 136, 0)
    struct.pack_into(">Q", sb, 144, 20)

    agf = memoryview(image)[sector_size : 2 * sector_size]
    agf[:4] = b"XAGF"
    struct.pack_into(">I", agf, 4, 1)
    struct.pack_into(">I", agf, 8, 0)
    struct.pack_into(">I", agf, 12, dblocks)
    struct.pack_into(">I", agf, 16, 4)
    struct.pack_into(">I", agf, 52, 20)
    struct.pack_into(">I", agf, 56, 20)

    agi = memoryview(image)[2 * sector_size : 3 * sector_size]
    agi[:4] = b"XAGI"
    struct.pack_into(">I", agi, 4, 1)
    struct.pack_into(">I", agi, 8, 0)
    struct.pack_into(">I", agi, 12, dblocks)
    struct.pack_into(">I", agi, 20, 5)

    bnobt = memoryview(image)[4 * block_size : 5 * block_size]
    bnobt[:4] = b"AB3B"
    struct.pack_into(">H", bnobt, 4, 0)
    struct.pack_into(">H", bnobt, 6, 1)
    struct.pack_into(">II", bnobt, 56, 100, 20)

    inobt = memoryview(image)[5 * block_size : 6 * block_size]
    inobt[:4] = b"IAB3"
    struct.pack_into(">H", inobt, 4, 0)
    struct.pack_into(">H", inobt, 6, 0)

    path.write_bytes(image)


def test_worker_identify_and_analysis() -> None:
    worker = _native_worker()
    os.environ["LINUX_DEFRAGGER_XFS_WORKER"] = str(worker)
    with tempfile.TemporaryDirectory() as raw:
        image = Path(raw) / "minimal-xfs.img"
        _minimal_xfs(image)
        identified = subprocess.run(
            [str(worker), "identify", str(image)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert identified.returncode == 0, identified.stderr
        assert json.loads(identified.stdout) == {"filesystem": "xfs"}

        analysed = subprocess.run(
            [str(worker), "analyse-json", str(image)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert analysed.returncode == 0, analysed.stderr
        payload = json.loads(analysed.stdout)
        assert payload["block_size"] == 4096
        assert payload["dblocks"] == 256
        assert payload["free_ranges"] == [[100, 120]]
        assert payload["bnobt_blocks"] == 1
        assert payload["inobt_blocks"] == 1


def test_gui_adapter_uses_native_analysis() -> None:
    worker = _native_worker()
    os.environ["LINUX_DEFRAGGER_XFS_WORKER"] = str(worker)
    with tempfile.TemporaryDirectory() as raw:
        image = Path(raw) / "minimal-xfs.img"
        _minimal_xfs(image)
        assert BACKEND.probe(str(image)) is True
        result = BACKEND.map(str(image), 32)
        assert result["filesystem"] == "xfs"
        assert result["map_accuracy"] == "exact"
        assert result["total_units"] == 256
        assert result["free_bytes"] == 20 * 4096
        assert result["regular_files"] == 0
        assert result["details"]["fragmentation_basis"].startswith("native C XFS")


def test_no_python_xfs_engine_remains() -> None:
    xfs_dir = GUI / "filesystems" / "xfs"
    python_files = sorted(path.name for path in xfs_dir.glob("*.py"))
    assert python_files == ["__init__.py", "plugin.py"]
    source = (xfs_dir / "plugin.py").read_text(encoding="utf-8")
    for forbidden in (
        "class _XfsGeometry",
        "_decode_bmbt_record",
        "RawXfsMetadata",
        "xfs_repair",
        "XFS_IOC_",
        "FIEMAP",
        "losetup",
        "mount -t xfs",
    ):
        assert forbidden not in source
    assert not (GUI / "xfs_engine.py").exists()


def main() -> None:
    test_worker_identify_and_analysis()
    test_gui_adapter_uses_native_analysis()
    test_no_python_xfs_engine_remains()
    print("native C XFS worker and thin GUI adapter tests passed")


if __name__ == "__main__":
    main()
