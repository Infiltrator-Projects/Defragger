#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end native HFS+/HFSX Defrag/Growth Defrag and journal-safety tests."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = BUILD / "linux-defragger-hfsplus-worker"
FIXTURE = ROOT / "tests" / "make_hfsplus_fixture.py"
sys.path.insert(0, str(ROOT / "tests"))
import make_hfsplus_fixture as fixture  # noqa: E402


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *map(str, args)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def analyse(path: Path) -> dict:
    return json.loads(run("analyse-json", path).stdout)


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(FIXTURE), str(path), *options], check=True)


def verify(path: Path, *, growth: bool = False) -> None:
    args = [sys.executable, str(FIXTURE), str(path), "--verify"]
    if growth:
        args.append("--growth")
    subprocess.run(args, check=True)


def mutate(path: Path, operation: str, *, live: bool = True) -> str:
    journal = Path(str(path) + f".{operation}.journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path, "--journal", journal,
        "--workers", "auto", "--ram-buffer", "auto", "--batch-clusters", "4096",
        "--live-map-cells", "512",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    if live:
        args += ["--live-updates"]
    completed = run(*args)
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert "allocated blocks only" in completed.stdout
    if live:
        assert "@@LIVE_RANGES " in completed.stdout
        assert "@@LIVE_RESET " in completed.stdout
    assert not journal.exists()
    return completed.stdout


def test_hfsplus_defrag(work: Path) -> None:
    image = work / "hfsplus.img"
    make(image)
    before = analyse(image)
    assert before["variant"] == "HFS+"
    assert before["regular_files"] == 2
    assert before["fragmented_files"] == 2
    mutate(image, "defrag")
    verify(image)
    after = analyse(image)
    assert after["fragmented_files"] == 0


def test_hfsx_growth(work: Path) -> None:
    image = work / "hfsx.img"
    make(image, "--hfsx")
    before = analyse(image)
    assert before["variant"] == "HFSX"
    assert before["fragmented_files"] == 2
    mutate(image, "growth-defrag")
    verify(image, growth=True)
    assert analyse(image)["fragmented_files"] == 0


def test_extents_overflow_growth(work: Path) -> None:
    image = work / "overflow.img"
    make(image, "--overflow")
    before = analyse(image)
    assert before["fragmented_files"] == 2
    mutate(image, "growth-defrag")
    verify(image, growth=True)
    after = analyse(image)
    assert after["fragmented_files"] == 0


def test_clean_journal_is_preserved(work: Path) -> None:
    image = work / "journaled.img"
    make(image, "--journaled")
    original = image.read_bytes()
    before = analyse(image)
    assert before["journaled"] is True and before["journal_empty"] is True
    assert before["regular_files"] == 2, before
    mutate(image, "growth-defrag")
    verify(image, growth=True)
    current = image.read_bytes()
    assert current[20 * fixture.BS:25 * fixture.BS] == original[20 * fixture.BS:25 * fixture.BS]
    after = analyse(image)
    assert after["journaled"] is True and after["journal_empty"] is True
    assert after["fragmented_files"] == 0


def test_dirty_journal_fails_closed(work: Path) -> None:
    image = work / "dirty-journal.img"
    make(image, "--journaled", "--dirty-journal")
    before = hashlib.sha256(image.read_bytes()).digest()
    journal = work / "dirty.journal"
    identified = run("identify", image)
    assert json.loads(identified.stdout)["filesystem"] == "hfsplus"
    completed = run(
        "defrag", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "pending transactions" in completed.stderr
    assert hashlib.sha256(image.read_bytes()).digest() == before
    assert not journal.exists()



def test_inconsistent_and_reserved_attributes_fail_closed(work: Path) -> None:
    for option, expected in (("--inconsistent", "marks the filesystem inconsistent"),
                             ("--reserved14", "reserved attribute bit 14")):
        image = work / (option[2:] + ".img")
        make(image, option)
        before = hashlib.sha256(image.read_bytes()).digest()
        journal = work / (option[2:] + ".journal")
        completed = run(
            "defrag", image, "--write", "--confirm", image, "--journal", journal,
            check=False,
        )
        assert completed.returncode != 0
        assert expected in completed.stderr
        assert hashlib.sha256(image.read_bytes()).digest() == before
        assert not journal.exists()


def test_recover_from_verified_stage(work: Path) -> None:
    image = work / "recover.img"
    make(image)
    mutate(image, "defrag", live=False)
    verify(image)
    stage = work / "recover.stage"
    shutil.copyfile(image, stage)
    # Damage one currently allocated user-data block in the source only.  Recover
    # must re-verify the persistent stage before copying it back.
    total, extents = fixture.parse_file_extents(image)[16]
    assert total and len(extents) == 1
    start = extents[0][0] * fixture.BS
    with image.open("r+b") as stream:
        stream.seek(start)
        stream.write(b"X" * fixture.BS)
        stream.flush()
        os.fsync(stream.fileno())
    journal = work / "recover.journal"
    journal.write_text(
        "LINUX-DEFRAGGER-HFSPLUS-1\n"
        f"device={image}\n"
        f"stage={stage}\n"
        "operation=defrag\n"
    )
    completed = run("recover", image, "--write", "--confirm", image, "--journal", journal)
    assert '@@RESULT {"operation":"recover","status":"completed"' in completed.stdout
    assert not stage.exists() and not journal.exists()
    verify(image)


def main() -> None:
    assert WORKER.is_file(), f"missing native HFS+ worker: {WORKER}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-hfsplus-") as directory:
        work = Path(directory)
        test_hfsplus_defrag(work)
        test_hfsx_growth(work)
        test_extents_overflow_growth(work)
        test_clean_journal_is_preserved(work)
        test_dirty_journal_fails_closed(work)
        test_inconsistent_and_reserved_attributes_fail_closed(work)
        test_recover_from_verified_stage(work)
    print("native HFS+/HFSX Defrag/Growth Defrag/journal/recovery tests passed")


if __name__ == "__main__":
    main()
