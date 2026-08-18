#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

for path in Path('cmake').rglob('*.cmake'):
    text = path.read_text(encoding='utf-8')
    changed = text.replace('fat_growth.c', 'fat_relayout.c').replace('fat_growth.h', 'fat_relayout.h')
    if changed != text:
        path.write_text(changed, encoding='utf-8')

print('Updated CMake references for shared FAT relayout source naming')
