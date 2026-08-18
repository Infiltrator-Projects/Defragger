#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep real EXT2/3/4 identity separate from shared backend routing."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog  # noqa: E402
from ui.devices import Volume  # noqa: E402
from ui.operation_planner import build_analysis_arguments  # noqa: E402


def catalog() -> BackendCatalog:
    return BackendCatalog.from_manifest(
        {
            "backends": [
                {
                    "id": "ext4",
                    "aliases": ["ext2", "ext3"],
                    "capabilities": 1,
                    "operations": [],
                }
            ]
        }
    )


def volume(variant: str) -> Volume:
    return Volume(
        catalog=catalog(),
        path=f"/dev/{variant}",
        name=variant,
        fstype=variant,
        label=f"LD_{variant.upper()}",
        size=2 * 1024 * 1024 * 1024,
        mountpoints=[],
        removable=True,
        readonly=False,
        model="",
        transport="mmc",
    )


def main() -> None:
    for variant in ("ext2", "ext3", "ext4"):
        item = volume(variant)
        # The shared implementation still routes through the ext4 backend id.
        assert item.normalized_fstype == "ext4"
        # User-facing identity must remain what was actually discovered.
        assert item.display_fstype == variant
        assert f"— {variant.upper()} —" in item.display_name
        arguments = build_analysis_arguments(
            "/mapper",
            item,
            4096,
            minimum_cells=1024,
            maximum_cells=500000,
        )
        assert arguments[3] == "ext4"

    coordinator = (GUI / "ui" / "operation_coordinator.py").read_text()
    assert "volume.display_fstype.upper()" in coordinator
    assert "Analysing {volume.normalized_fstype.upper()}" not in coordinator
    print("EXT display identity remains separate from shared backend routing")


if __name__ == "__main__":
    main()
