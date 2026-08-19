#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native Amiga OFS/FFS mutation and recovery-safety regression tests."""

from __future__ import annotations

import gzip
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = BUILD / "linux-defragger-affs-worker"
BLOCK_SIZE = 512


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


def analyse(path: Path) -> dict:
    return json.loads(run("analyse-json", path).stdout)


def fixture(name: str, directory: str | Path) -> Path:
    source = ROOT / "tests" / "fixtures" / name
    output = Path(directory) / name.removesuffix(".gz")
    with gzip.open(source, "rb") as input_file, output.open("wb") as output_file:
        shutil.copyfileobj(input_file, output_file)
    return output


def allocated_blocks(path: Path) -> list[int]:
    info = analyse(path)
    total = int(info["total_blocks"])
    free = [False] * total
    for start, end in info["free_ranges"]:
        for block in range(int(start), int(end)):
            free[block] = True
    return [block for block, is_free in enumerate(free) if not is_free]


def stage_digest(path: Path) -> str:
    data = path.read_bytes()
    digest = hashlib.sha256()
    for block in allocated_blocks(path):
        digest.update(data[block * BLOCK_SIZE:(block + 1) * BLOCK_SIZE])
    return digest.hexdigest()


def transaction_fields(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    st = path.stat()
    blocks = st.st_size // BLOCK_SIZE
    root = blocks // 2
    token = hashlib.sha256(
        data[0:BLOCK_SIZE]
        + data[BLOCK_SIZE:2 * BLOCK_SIZE]
        + data[root * BLOCK_SIZE:(root + 1) * BLOCK_SIZE]
    ).hexdigest()
    return {
        "device": str(path.resolve()),
        "target_identity": f"file:{st.st_dev}:{st.st_ino}",
        "volume_token": token,
        "physical_bytes": st.st_size,
        "filesystem_bytes": blocks * BLOCK_SIZE,
        "blocks": blocks,
        "root_block": root,
        "dostype": data[3],
    }


def write_recovery_journal(journal: Path, image: Path, stage: Path,
                           operation: str = "defrag", phase: str = "committing") -> None:
    fields = transaction_fields(image)
    journal.write_text(
        "LINUX-DEFRAGGER-AFFS-JOURNAL-2\n"
        f"device={fields['device']}\n"
        f"target_identity={fields['target_identity']}\n"
        f"stage={stage}\n"
        f"operation={operation}\n"
        f"phase={phase}\n"
        f"volume_token={fields['volume_token']}\n"
        f"stage_sha256={stage_digest(stage)}\n"
        f"physical_bytes={fields['physical_bytes']}\n"
        f"filesystem_bytes={fields['filesystem_bytes']}\n"
        f"blocks={fields['blocks']}\n"
        f"root_block={fields['root_block']}\n"
        f"dostype={fields['dostype']}\n",
        encoding="utf-8",
    )


def mutate(path: Path, operation: str) -> None:
    journal = Path(str(path) + ".journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path,
        "--journal", journal, "--live-updates",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    completed = run(*args)
    assert "@@LIVE_RANGES" in completed.stdout
    assert "@@LIVE_RESET" in completed.stdout
    assert "allocated blocks only" in completed.stdout
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert not journal.exists()
    assert not Path(str(journal) + ".affs-stage").exists()


def test_mutation_paths(work: Path) -> None:
    cases = [
        ("affs-ffs-fragmented.adf.gz", "FFS", "defrag"),
        ("affs-ofs-fragmented.adf.gz", "OFS", "growth-defrag"),
    ]
    for name, variant, operation in cases:
        path = fixture(name, work)
        before = analyse(path)
        assert before["variant"] == variant
        assert before["fragmented_files"] > 0
        mutate(path, operation)
        after = analyse(path)
        assert after["fragmented_files"] == 0
        run("identify", path)


def make_defragged_source(work: Path, stem: str) -> Path:
    path = fixture("affs-ffs-fragmented.adf.gz", work)
    renamed = work / f"{stem}.adf"
    path.replace(renamed)
    mutate(renamed, "defrag")
    return renamed


def test_recover_from_verified_bound_stage(work: Path) -> None:
    image = make_defragged_source(work, "recover")
    stage = work / "recover.stage"
    shutil.copyfile(image, stage)
    expected = hashlib.sha256(stage.read_bytes()).digest()
    fields = transaction_fields(image)
    candidates = [
        block for block in allocated_blocks(image)
        if block not in {0, 1, int(fields["root_block"])}
    ]
    assert candidates
    block = candidates[-1]
    with image.open("r+b") as stream:
        stream.seek(block * BLOCK_SIZE)
        original = stream.read(1)
        stream.seek(block * BLOCK_SIZE)
        stream.write(bytes([original[0] ^ 0xFF]))
        stream.flush()
        os.fsync(stream.fileno())
    journal = work / "recover.journal"
    write_recovery_journal(journal, image, stage)
    completed = run("recover", image, "--write", "--confirm", image, "--journal", journal)
    assert '@@RESULT {"operation":"recover","status":"completed"' in completed.stdout
    assert hashlib.sha256(image.read_bytes()).digest() == expected
    assert not journal.exists() and not stage.exists()


def test_corrupt_stage_is_never_committed(work: Path) -> None:
    image = make_defragged_source(work, "corrupt-source")
    stage = work / "corrupt.stage"
    shutil.copyfile(image, stage)
    journal = work / "corrupt.journal"
    write_recovery_journal(journal, image, stage)
    before = hashlib.sha256(image.read_bytes()).digest()
    fields = transaction_fields(stage)
    candidates = [
        block for block in allocated_blocks(stage)
        if block not in {0, 1, int(fields["root_block"])}
    ]
    assert candidates
    with stage.open("r+b") as stream:
        stream.seek(candidates[-1] * BLOCK_SIZE)
        original = stream.read(1)
        stream.seek(candidates[-1] * BLOCK_SIZE)
        stream.write(bytes([original[0] ^ 0x5A]))
    completed = run(
        "recover", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    # Corrupt allocated metadata can be rejected by the independent AFFS parser
    # before the persisted SHA-256 comparison is reached.  Either rejection path
    # is correct; the safety invariant is that no source byte is written.
    assert completed.returncode != 0
    assert completed.stderr.strip()
    assert hashlib.sha256(image.read_bytes()).digest() == before
    assert journal.exists() and stage.exists()


def test_recovery_refuses_different_target(work: Path) -> None:
    original = make_defragged_source(work, "identity-original")
    other = fixture("affs-ffs-fragmented.adf.gz", work)
    other = other.rename(work / "identity-other.adf")
    mutate(other, "defrag")
    stage = work / "identity.stage"
    shutil.copyfile(original, stage)
    journal = work / "identity.journal"
    write_recovery_journal(journal, original, stage)
    before = hashlib.sha256(other.read_bytes()).digest()
    completed = run(
        "recover", other, "--write", "--confirm", other, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "target path, identity or capacity changed" in completed.stderr
    assert hashlib.sha256(other.read_bytes()).digest() == before
    assert journal.exists() and stage.exists()


def test_legacy_recovery_journal_fails_closed(work: Path) -> None:
    image = fixture("affs-ffs-fragmented.adf.gz", work)
    stage = work / "legacy.stage"
    shutil.copyfile(image, stage)
    journal = work / "legacy.journal"
    journal.write_text(
        "LINUX-DEFRAGGER-AFFS-1\n"
        f"device={image}\n"
        f"stage={stage}\n"
        "operation=defrag\n",
        encoding="utf-8",
    )
    completed = run(
        "recover", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "lacks target identity and stage integrity" in completed.stderr


def main() -> None:
    assert WORKER.is_file(), f"missing native Amiga worker: {WORKER}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-affs-") as directory:
        work = Path(directory)
        test_mutation_paths(work)
        test_recover_from_verified_bound_stage(work)
        test_corrupt_stage_is_never_committed(work)
        test_recovery_refuses_different_target(work)
        test_legacy_recovery_journal_fails_closed(work)
    print("native Amiga OFS/FFS Defrag/Growth Defrag/identity/integrity recovery tests passed")


if __name__ == "__main__":
    main()
