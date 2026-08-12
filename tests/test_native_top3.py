#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end image tests for the current C-first EXT, NTFS and exFAT C engines."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
sys.path.insert(0, str(ROOT / "tests"))
from ntfs_test_fixture import make_image as make_ntfs_image  # noqa: E402


def run_json(worker: Path, image: Path) -> dict:
    completed = subprocess.run(
        [str(worker), "analyse-json", str(image)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)


def mutate(worker: Path, image: Path, operation: str, journal: Path) -> str:
    arguments = [
        str(worker), operation, str(image), "--write", "--confirm", str(image),
        "--journal", str(journal), "--batch-clusters", "4096",
        "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
    ]
    if operation == "growth-defrag":
        arguments.extend(("--growth-percent", "10"))
    completed = subprocess.run(
        arguments,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert "@@LIVE_RESET " in completed.stdout
    assert not journal.exists()
    return completed.stdout


def assert_clean(payload: dict, *, growth: bool) -> None:
    assert payload["fragmented_files"] == 0, payload
    assert payload["fragmented_directories"] == 0, payload
    if growth:
        assert payload["growth_10_satisfied"] is True, payload


def test_exfat(work: Path) -> None:
    worker = BUILD / "linux-defragger-exfat-worker"
    image = work / "exfat.img"
    subprocess.run([sys.executable, str(ROOT / "tests" / "make_exfat_image.py"), str(image)], check=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0 and before["fragmented_directories"] > 0
    serial = before["serial"]
    output = mutate(worker, image, "defrag", work / "exfat-defrag.journal")
    assert "@@LIVE_RANGES " in output, output
    assert "verified allocated/metadata ranges instead of rewriting the full" in output, output
    packed = run_json(worker, image)
    assert packed["serial"] == serial
    assert_clean(packed, growth=False)
    output = mutate(worker, image, "growth-defrag", work / "exfat-growth.journal")
    assert "@@LIVE_RANGES " in output, output
    assert "verified allocated/metadata ranges instead of rewriting the full" in output, output
    grown = run_json(worker, image)
    assert grown["serial"] == serial
    assert_clean(grown, growth=True)


def test_ntfs(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    image = work / "ntfs.img"
    make_ntfs_image(image, fragmented_data=True, directory_data=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0
    serial = before["serial"]
    mutate(worker, image, "defrag", work / "ntfs-defrag.journal")
    packed = run_json(worker, image)
    assert packed["serial"] == serial
    assert_clean(packed, growth=False)
    mutate(worker, image, "growth-defrag", work / "ntfs-growth.journal")
    grown = run_json(worker, image)
    assert grown["serial"] == serial
    assert_clean(grown, growth=True)



def test_ntfs_preserves_safe_unsupported_user_stream(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    image = work / "ntfs-fixed-user-stream.img"
    make_ntfs_image(
        image,
        fragmented_data=True,
        directory_data=True,
        fixed_attribute_list_stream=True,
    )
    from ntfs_test_fixture import FIXED_USER_LCN, FIXED_USER_CLUSTERS, CLUSTER_SIZE
    start = FIXED_USER_LCN * CLUSTER_SIZE
    length = FIXED_USER_CLUSTERS * CLUSTER_SIZE
    before_fixed = image.read_bytes()[start:start + length]
    completed = subprocess.run(
        [
            str(worker), "defrag", str(image), "--write", "--confirm", str(image),
            "--journal", str(work / "ntfs-fixed.journal"), "--batch-clusters", "4096",
            "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert "Preserving 1 unsupported-but-safe NTFS user stream" in completed.stdout, completed.stdout
    after_fixed = image.read_bytes()[start:start + length]
    assert after_fixed == before_fixed
    assert_clean(run_json(worker, image), growth=False)

    growth = subprocess.run(
        [
            str(worker), "growth-defrag", str(image), "--write", "--confirm", str(image),
            "--journal", str(work / "ntfs-fixed-growth.journal"), "--batch-clusters", "4096",
            "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
            "--growth-percent", "10",
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert "Preserving 1 unsupported-but-safe NTFS user stream" in growth.stdout, growth.stdout
    assert image.read_bytes()[start:start + length] == before_fixed
    assert_clean(run_json(worker, image), growth=True)

def test_ext(work: Path) -> None:
    worker = BUILD / "linux-defragger-ext-worker"
    fixture = BUILD / "linux-defragger-ext-fixture"
    image = work / "ext4.img"
    subprocess.run([str(fixture), str(image)], check=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0 or before["fragmented_directories"] > 0
    identity = before["uuid"]
    output = mutate(worker, image, "defrag", work / "ext-defrag.journal")
    match = re.search(
        r"EXT source commit: writing ([0-9.]+) MB of verified allocated blocks "
        r"instead of rewriting the full ([0-9.]+) MB filesystem\.",
        output,
    )
    assert match is not None, output
    committed_mb, full_mb = map(float, match.groups())
    assert committed_mb < full_mb * 0.50, (committed_mb, full_mb, output)
    packed = run_json(worker, image)
    assert packed["uuid"] == identity
    assert_clean(packed, growth=False)
    output = mutate(worker, image, "growth-defrag", work / "ext-growth.journal")
    match = re.search(
        r"EXT source commit: writing ([0-9.]+) MB of verified allocated blocks "
        r"instead of rewriting the full ([0-9.]+) MB filesystem\.",
        output,
    )
    assert match is not None, output
    committed_mb, full_mb = map(float, match.groups())
    assert committed_mb < full_mb * 0.50, (committed_mb, full_mb, output)
    grown = run_json(worker, image)
    assert grown["uuid"] == identity
    assert_clean(grown, growth=True)


def main() -> None:
    for name in ("linux-defragger-ext-worker", "linux-defragger-ntfs-worker",
                 "linux-defragger-exfat-worker", "linux-defragger-ext-fixture"):
        assert (BUILD / name).is_file(), f"missing native test executable: {name}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-native83-") as directory:
        work = Path(directory)
        test_exfat(work)
        test_ntfs(work)
        test_ntfs_preserves_safe_unsupported_user_stream(work)
        test_ext(work)
    print("native EXT, NTFS and exFAT Defrag/Growth Defrag tests passed")


if __name__ == "__main__":
    main()
