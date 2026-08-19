#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Btrfs allocation, fragmentation and adapter regression tests."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = Path(os.environ.get(
    "LINUX_DEFRAGGER_BTRFS_WORKER", BUILD / "linux-defragger-btrfs-worker"
))
FIXTURE = ROOT / "tests" / "make_btrfs_fixture.py"
sys.path.insert(0, str(ROOT / "gui"))

from filesystems.btrfs.plugin import BtrfsBackend  # noqa: E402


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *map(str, args)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(FIXTURE), str(path), *options], check=True)


def test_worker_contract(work: Path) -> None:
    image = work / "valid.img"
    make(image)
    identified = json.loads(run("identify", image).stdout)
    assert identified == {"filesystem": "btrfs"}

    summary = json.loads(run("analyse-json", image).stdout)
    assert summary["filesystem"] == "btrfs"
    assert summary["sector_size"] == 4096
    assert summary["node_size"] == 4096
    assert summary["filesystem_bytes"] == 8 * 1024 * 1024
    assert summary["physical_bytes"] == 10 * 1024 * 1024
    assert summary["regular_files"] == 1
    assert summary["directories"] == 1
    assert summary["fragmented_files"] == 1
    assert summary["fragmented_directories"] == 0
    assert summary["malformed_items"] == 0

    result = json.loads(run("map", image, "--cells", "16").stdout)
    assert result["schema"] == 1
    assert result["backend"] == "read-only-domain"
    assert result["filesystem"] == "btrfs"
    assert result["map_accuracy"] == "exact-single-device"
    assert result["unit_size"] == 4096
    assert result["filesystem_units"] == 2048
    assert result["total_units"] == 2560
    assert result["outside_bytes"] == 2 * 1024 * 1024
    assert result["unknown_bytes"] == 0
    assert result["used_bytes"] == 7 * 4096
    assert result["free_bytes"] == (2048 - 7) * 4096
    assert result["regular_files"] == 1
    assert result["directories"] == 1
    assert result["fragmented_files"] == 1
    assert result["details"]["fragmentation_available"] is True
    assert result["details"]["fragmented_sectors_mapped"] == 2
    assert sum(cell["outside"] for cell in result["cells"]) == 512
    assert sum(cell["fragmented"] for cell in result["cells"]) == 2


def test_python_adapter_is_native_only(work: Path) -> None:
    image = work / "adapter.img"
    make(image)
    old = os.environ.get("LINUX_DEFRAGGER_BTRFS_WORKER")
    os.environ["LINUX_DEFRAGGER_BTRFS_WORKER"] = str(WORKER)
    try:
        backend = BtrfsBackend()
        assert backend.probe(str(image))
        result = backend.map(str(image), 12)
    finally:
        if old is None:
            os.environ.pop("LINUX_DEFRAGGER_BTRFS_WORKER", None)
        else:
            os.environ["LINUX_DEFRAGGER_BTRFS_WORKER"] = old
    assert result["fragmented_files"] == 1
    assert result["outside_bytes"] == 2 * 1024 * 1024

    source = (ROOT / "gui" / "filesystems" / "btrfs" / "plugin.py").read_text()
    for forbidden in (
        "Reader", "u16le", "u32le", "u64le", "bisect", "_TreeReader",
        "_Mapper", "_CHUNK_ITEM", "_EXTENT_ITEM", "_FILE_EXTENT_REG",
        "aggregate_ranges", "complement_ranges", "overlay_ranges",
    ):
        assert forbidden not in source, forbidden
    assert 'resolve_program("btrfs-native"' in source
    assert len(source.splitlines()) < 180


def test_malformed_metadata_fails_closed(work: Path) -> None:
    image = work / "malformed.img"
    make(image, "--malformed")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "overlaps its item table" in completed.stderr


def test_unsupported_layouts_fail_closed(work: Path) -> None:
    image = work / "multi.img"
    make(image, "--multi-device")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "single-device" in completed.stderr

    image = work / "striped.img"
    make(image, "--striped")
    completed = run("map", image, "--cells", "8", check=False)
    assert completed.returncode != 0
    assert "striped Btrfs profiles" in completed.stderr


def main() -> None:
    assert WORKER.is_file(), f"missing native Btrfs worker: {WORKER}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-btrfs-") as directory:
        work = Path(directory)
        test_worker_contract(work)
        test_python_adapter_is_native_only(work)
        test_malformed_metadata_fails_closed(work)
        test_unsupported_layouts_fail_closed(work)
    print("native Btrfs allocation/fragmentation/adapter tests passed")


if __name__ == "__main__":
    main()
