#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

root = Path(__file__).resolve().parents[1]
native = root / "gui/filesystems/fat/native"
writer = (native / "writer.c").read_text(encoding="utf-8")
planner = (native / "fat_relayout.c").read_text(encoding="utf-8")
header = (native / "fat_relayout.h").read_text(encoding="utf-8")
analysis = (native / "fat_analysis.c").read_text(encoding="utf-8")

assert not (native / "fat_growth.c").exists()
assert not (native / "fat_growth.h").exists()
for token in ("FAT_TYPE_12", "FAT_TYPE_16", "FAT_TYPE_32"):
    assert token not in writer, f"relayout executor branches on {token}"
    assert token not in planner, f"relayout planner branches on {token}"

assert writer.count("fat_relayout_volume(&fs") >= 2, "Defrag and Growth Defrag must share fat_relayout_volume"
assert "fat_relayout_volume(&fs, journal_path, 0," in writer, "Defrag must be reserve=0"
assert "fat_relayout_volume(&fs, journal_path, growth_percent," in writer, "Growth Defrag must pass its reserve policy"
assert "FatRelayoutObject" in header and "FatRelayoutStats" in header
assert "fat_relayout_plan_layout" in planner
assert "fat_relayout_matches_canonical" in analysis

legacy = ("GrowthObject", "GrowthObjectList", "GrowthStats", "GrowthPreflight",
          "build_growth_objects", "plan_growth_layout", "growth_layout_preflight",
          "growth_layout_matches_canonical")
for token in legacy:
    assert token not in writer, f"legacy growth-only executor symbol remains: {token}"
    assert token not in planner, f"legacy growth-only planner symbol remains: {token}"

for bits in (12, 16, 32):
    plugin = (root / f"gui/filesystems/fat{bits}/plugin.py").read_text(encoding="utf-8")
    assert f"FatBackend({bits})" in plugin

print("FAT12/FAT16/FAT32 share one width-neutral relayout planner and executor")
