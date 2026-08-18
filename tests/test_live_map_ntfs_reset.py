#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression coverage for NTFS live allocation-map reset events."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.live_map import LiveMapUpdater


def _map() -> dict:
    return {
        "filesystem": "ntfs",
        "unit_size": 4096,
        "total_units": 16,
        "filesystem_units": 16,
        "cells": [
            {
                "start": 0,
                "end": 7,
                "free": 0,
                "used": 8,
                "unknown": 0,
                "outside": 0,
                "fragmented": 4,
                "directory": 1,
                "bad": 0,
            },
            {
                "start": 8,
                "end": 15,
                "free": 0,
                "used": 8,
                "unknown": 0,
                "outside": 0,
                "fragmented": 2,
                "directory": 0,
                "bad": 0,
            },
        ],
    }


def test_standard_live_reset() -> None:
    data = _map()
    LiveMapUpdater(data).reset(
        {
            "filesystem": "ntfs",
            "unit_size": 4096,
            "filesystem_units": 16,
            "used_ranges": [[0, 4 * 4096], [8 * 4096, 4 * 4096]],
        }
    )
    assert [(cell["used"], cell["free"]) for cell in data["cells"]] == [
        (4, 4),
        (4, 4),
    ]
    assert all(cell["fragmented"] == 0 for cell in data["cells"])


def test_ntfs_126_legacy_live_reset() -> None:
    data = _map()
    LiveMapUpdater(data).reset(
        {
            "filesystem": "ntfs",
            "block_size": 4096,
            "total_blocks": 16,
            "free_ranges": [[4, 8], [12, 16]],
        }
    )
    assert [(cell["used"], cell["free"]) for cell in data["cells"]] == [
        (4, 4),
        (4, 4),
    ]
    assert data["filesystem_units"] == 16
    assert data["filesystem_bytes"] == 16 * 4096


def main() -> None:
    test_standard_live_reset()
    test_ntfs_126_legacy_live_reset()
    print("NTFS live-map reset regression tests passed")


if __name__ == "__main__":
    main()
