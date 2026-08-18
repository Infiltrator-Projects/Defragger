#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Branch-only refactor helper: make FAT12/16/32 relayout naming explicitly shared."""

from __future__ import annotations

from pathlib import Path

ROOT = Path('.')
OLD_C = ROOT / 'gui/filesystems/fat/native/fat_growth.c'
OLD_H = ROOT / 'gui/filesystems/fat/native/fat_growth.h'
NEW_C = ROOT / 'gui/filesystems/fat/native/fat_relayout.c'
NEW_H = ROOT / 'gui/filesystems/fat/native/fat_relayout.h'

if not OLD_C.exists() or not OLD_H.exists():
    raise SystemExit('legacy FAT growth planner files were not found')
if NEW_C.exists() or NEW_H.exists():
    raise SystemExit('FAT relayout planner files already exist')

replacements = [
    ('LINUX_DEFRAGGER_FAT_GROWTH_H', 'LINUX_DEFRAGGER_FAT_RELAYOUT_H'),
    ('fat_growth.h', 'fat_relayout.h'),
    ('fat_growth.c', 'fat_relayout.c'),
    ('GrowthPreflightIssue', 'FatRelayoutPreflightIssue'),
    ('GrowthPreflight', 'FatRelayoutPreflight'),
    ('GrowthObjectList', 'FatRelayoutObjectList'),
    ('GrowthObject', 'FatRelayoutObject'),
    ('GrowthStats', 'FatRelayoutStats'),
    ('GROWTH_PREFLIGHT_', 'FAT_RELAYOUT_PREFLIGHT_'),
    ('growth_object_list_free', 'fat_relayout_object_list_free'),
    ('build_growth_objects', 'fat_relayout_build_objects'),
    ('plan_growth_layout', 'fat_relayout_plan_layout'),
    ('growth_layout_preflight', 'fat_relayout_preflight'),
    ('growth_preflight_free', 'fat_relayout_preflight_free'),
    ('growth_layout_matches_canonical', 'fat_relayout_matches_canonical'),
    ('print_growth_preflight_failure', 'fat_relayout_print_preflight_failure'),
    ('growth_object_list_push', 'relayout_object_list_push'),
    ('compare_growth_objects_asc', 'compare_relayout_objects_asc'),
    ('growth_cluster_is_barrier', 'relayout_cluster_is_barrier'),
    ('growth_find_usable_run', 'relayout_find_usable_run'),
    ('growth_advance_reserve', 'relayout_advance_reserve'),
    ('growth_object_cluster_total', 'relayout_object_cluster_total'),
    ('find_growth_object_chain', 'find_relayout_object_chain'),
    ('growth_move_chain', 'relayout_move_chain'),
    ('automatic_growth_batch_clusters', 'automatic_relayout_batch_clusters'),
    ('growth_batch_can_add', 'relayout_batch_can_add'),
]

# Rename the pure planning module first, then update every repository text file
# that can legitimately refer to its public names or source path.
NEW_C.write_text(OLD_C.read_text(encoding='utf-8'), encoding='utf-8')
NEW_H.write_text(OLD_H.read_text(encoding='utf-8'), encoding='utf-8')
OLD_C.unlink()
OLD_H.unlink()

text_names = {'CMakeLists.txt', 'Makefile'}
text_suffixes = {'.c', '.h', '.py', '.sh', '.md', '.txt', '.yml', '.yaml'}
for path in ROOT.rglob('*'):
    if not path.is_file():
        continue
    if '.git' in path.parts or 'shared' in path.parts:
        continue
    if path.name not in text_names and path.suffix not in text_suffixes:
        continue
    try:
        text = path.read_text(encoding='utf-8')
    except UnicodeDecodeError:
        continue
    changed = text
    for old, new in replacements:
        changed = changed.replace(old, new)
    if path == NEW_C:
        changed = changed.replace(
            '/* FAT canonical growth-layout model and pure planner. */',
            '/* Shared FAT12/FAT16/FAT32 canonical relayout model and pure planner. */',
            1,
        )
        changed = changed.replace(
            '/* Growth Defrag is intended to be idempotent.  The preflight is deliberately',
            '/* Canonical FAT relayout is idempotent for both zero-gap Defragment and\n   reserve-bearing Growth Defrag.  The preflight is deliberately',
            1,
        )
        changed = changed.replace(
            '/* A second, independent idempotence check recognises the exact canonical\n   physical layout produced by Growth Defrag.',
            '/* A second, independent idempotence check recognises the exact canonical\n   physical layout produced by the shared FAT relayout planner.',
            1,
        )
    if path == NEW_H:
        changed = changed.replace(
            '#define LINUX_DEFRAGGER_FAT_RELAYOUT_H\n',
            '#define LINUX_DEFRAGGER_FAT_RELAYOUT_H\n\n'
            '/* This public model is deliberately FAT-width neutral.  FAT12, FAT16 and\n'
            '   FAT32 geometry stays in Fat32/FatGeometry; relayout policy is only the\n'
            '   requested post-file reserve percentage. */\n',
            1,
        )
    if path.name == 'writer.c':
        changed = changed.replace(
            'requested_percent == 0 ? " and canonical zero-gap packing" :\n'
            '                                     " with a 10% post-file reserve");',
            'requested_percent == 0 ? " and canonical zero-gap packing" :\n'
            '                                     " with the requested post-file reserve");',
            1,
        )
        changed = changed.replace(
            '"%s preflight result: the existing FAT layout already satisfies a 10%% "',
            '"%s preflight result: the existing FAT layout already satisfies the requested "',
            1,
        )
        changed = changed.replace(
            '"post-file reserve for %zu regular file%s; %zu director%s %s contiguous.\\n",\n'
            '                    layout_name, regular_files,',
            '"%u%% post-file reserve for %zu regular file%s; %zu director%s %s contiguous.\\n",\n'
            '                    layout_name, requested_percent, regular_files,',
            1,
        )
        changed = changed.replace(
            '"Growth Defrag remaining dependency set exceeds the all-at-once workspace fast path; switching to the adaptive dependency scheduler.\\n"',
            '"%s remaining dependency set exceeds the all-at-once workspace fast path; switching to the adaptive dependency scheduler.\\n", layout_name',
            1,
        )
        changed = changed.replace(
            'ld_die("growth-defrag staged object did not reopen at the workspace")',
            'ld_die("FAT relayout staged object did not reopen at the workspace")',
        )
        changed = changed.replace(
            'tiny FAT16 files still batch by the thousand,',
            'tiny FAT objects still batch by the thousand,',
        )
    if changed != text:
        path.write_text(changed, encoding='utf-8')

# Add an architecture regression that makes the intended sharing executable:
# the planner/executor may not branch on FAT width, both public operations must
# call the same relayout entry point, and no legacy growth-only planner API may
# creep back into the native implementation.
arch_test = ROOT / 'tests/test_fat_relayout_architecture.py'
arch_test.write_text('''#!/usr/bin/env python3\n# SPDX-License-Identifier: GPL-3.0-or-later\nfrom pathlib import Path\n\nroot = Path(__file__).resolve().parents[1]\nnative = root / "gui/filesystems/fat/native"\nwriter = (native / "writer.c").read_text(encoding="utf-8")\nplanner = (native / "fat_relayout.c").read_text(encoding="utf-8")\nheader = (native / "fat_relayout.h").read_text(encoding="utf-8")\nanalysis = (native / "fat_analysis.c").read_text(encoding="utf-8")\n\nassert not (native / "fat_growth.c").exists()\nassert not (native / "fat_growth.h").exists()\nfor token in ("FAT_TYPE_12", "FAT_TYPE_16", "FAT_TYPE_32"):\n    assert token not in writer, f"relayout executor branches on {token}"\n    assert token not in planner, f"relayout planner branches on {token}"\n\nassert writer.count("fat_relayout_volume(&fs") >= 2, "Defrag and Growth Defrag must share fat_relayout_volume"\nassert "fat_relayout_volume(&fs, journal_path, 0," in writer, "Defrag must be reserve=0"\nassert "fat_relayout_volume(&fs, journal_path, growth_percent," in writer, "Growth Defrag must pass its reserve policy"\nassert "FatRelayoutObject" in header and "FatRelayoutStats" in header\nassert "fat_relayout_plan_layout" in planner\nassert "fat_relayout_matches_canonical" in analysis\n\nlegacy = ("GrowthObject", "GrowthObjectList", "GrowthStats", "GrowthPreflight",\n          "build_growth_objects", "plan_growth_layout", "growth_layout_preflight",\n          "growth_layout_matches_canonical")\nfor token in legacy:\n    assert token not in writer, f"legacy growth-only executor symbol remains: {token}"\n    assert token not in planner, f"legacy growth-only planner symbol remains: {token}"\n\nfor bits in (12, 16, 32):\n    plugin = (root / f"gui/filesystems/fat{bits}/plugin.py").read_text(encoding="utf-8")\n    assert f"FatBackend({bits})" in plugin\n\nprint("FAT12/FAT16/FAT32 share one width-neutral relayout planner and executor")\n''', encoding='utf-8')

print('Applied shared FAT relayout naming and architecture invariants')
