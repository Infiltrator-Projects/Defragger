#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test the standard read-only analysis registry and mapper interface."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
sys.path.insert(0, str(GUI))

from backends.registry import Registry

registry = Registry()
manifest = registry.manifest()
assert len(manifest) == 17
assert all(item["capabilities"] & 1 for item in manifest)
assert all(item["capabilities"] & 2 for item in manifest)

mapper = GUI / "allocation_mapper.py"
environment = {**os.environ, "PYTHONPATH": str(GUI), "PYTHONDONTWRITEBYTECODE": "1"}
result = subprocess.run(
    [sys.executable, str(mapper), "--list-backends"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=environment,
    check=True,
)
listed = json.loads(result.stdout)["backends"]
assert [item["id"] for item in listed] == [item["id"] for item in manifest]

invalid = subprocess.run(
    [sys.executable, str(mapper), "/dev/null", "--fstype", "ntfs", "--cells", "128"],
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=environment,
)
assert invalid.returncode != 0
assert invalid.stderr.strip()


# Btrfs filesystems can be smaller than their containing partition.  The
# helper must remain part of the plugin when the analyser is slimmed down.
from backends import btrfs
synthetic = {
    "cells": [
        {"start": 0, "end": 4, "unknown": 0},
        {"start": 5, "end": 9, "unknown": 5},
    ],
    "unknown_bytes": 5 * 4096,
    "details": {},
}
marked = btrfs._mark_outside_tail(synthetic, 5, 10, 4096)
assert marked["cells"][0].get("outside", 0) == 0
assert marked["cells"][1]["outside"] == 5
assert marked["cells"][1]["unknown"] == 0
assert marked["outside_bytes"] == 5 * 4096
assert marked["unknown_bytes"] == 0

print("standard read-only plugin and mapper smoke tests passed")
