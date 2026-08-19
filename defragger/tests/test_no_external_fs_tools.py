#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if production filesystem code regresses to external FS tool orchestration."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Production may use normal OS services (raw pread/pwrite, block-device ioctl,
# privilege separation) but must never outsource filesystem mutation/repair to
# command-line filesystem utilities or mount a filesystem to make changes.
FORBIDDEN_TOKENS = {
    "e2image",
    "resize2fs",
    "e2fsck",
    "xfs_repair",
    "xfs_db",
    "xfs_fsr",
    "ntfsfix",
    "ntfsresize",
    "ntfsclone",
    "ntfs-3g",
    "fsck.exfat",
    "exfatfsck",
    "btrfs filesystem defragment",
    "btrfs check",
    "losetup",
}

PRODUCTION_ROOTS = [
    ROOT / "gui",
    ROOT / "src",
    ROOT / "packaging",
]
PRODUCTION_FILES = [
    ROOT / "CMakeLists.txt",
    ROOT / "install.sh",
]


def iter_production_files():
    for root in PRODUCTION_ROOTS:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in {".py", ".c", ".h", ".sh"}:
                yield path
    yield from PRODUCTION_FILES


def main() -> None:
    offenders: list[str] = []
    for path in iter_production_files():
        text = path.read_text(encoding="utf-8", errors="replace").lower()
        for token in FORBIDDEN_TOKENS:
            if token in text:
                offenders.append(f"{path.relative_to(ROOT)}: {token}")
    if offenders:
        rendered = "\n".join(f"  - {item}" for item in offenders)
        raise AssertionError(
            "production code must implement filesystems itself; external filesystem "
            f"tool orchestration was found:\n{rendered}"
        )
    print("production filesystem external-tool prohibition passed")


if __name__ == "__main__":
    main()
