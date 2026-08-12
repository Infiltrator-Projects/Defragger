#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""C-first planner architecture check.

The mutating EXT, NTFS and exFAT planners are tested through their native
end-to-end fixtures. This file prevents Python planner implementations from
silently returning.
"""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
for fs in ("ext4", "ntfs", "exfat"):
    package = ROOT / "gui" / "filesystems" / fs
    assert not (package / "planner.py").exists()
    assert (package / "native").is_dir()
print("C-first planner ownership test passed")
