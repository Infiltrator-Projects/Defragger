#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

old_test = Path('tests/test_fat_growth.c')
new_test = Path('tests/test_fat_relayout.c')
if old_test.exists() and not new_test.exists():
    old_test.rename(new_test)

for path in Path('cmake').rglob('*.cmake'):
    text = path.read_text(encoding='utf-8')
    changed = text.replace('fat_growth.c', 'fat_relayout.c').replace('fat_growth.h', 'fat_relayout.h')
    changed = changed.replace('test_fat_growth.c', 'test_fat_relayout.c')
    changed = changed.replace('linux-defragger-fat-growth-test', 'linux-defragger-fat-relayout-model-test')
    changed = changed.replace('NAME linux-defragger-fat-growth\n', 'NAME linux-defragger-fat-relayout-model\n')
    if changed != text:
        path.write_text(changed, encoding='utf-8')

print('Updated CMake and model-test references for shared FAT relayout naming')
