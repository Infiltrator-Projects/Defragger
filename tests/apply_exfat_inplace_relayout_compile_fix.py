#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path

path = Path('gui/filesystems/exfat/native/exfat_relayout.c')
text = path.read_text(encoding='utf-8')
anchor = '#include "ld_io.h"\n'
replacement = '#include "ld_device.h"\n#include "ld_io.h"\n'
if '#include "ld_device.h"\n' not in text:
    if anchor not in text:
        raise SystemExit('ld_io include anchor not found')
    text = text.replace(anchor, replacement, 1)
path.write_text(text, encoding='utf-8')
print('Added ld_device.h to exFAT relayout engine')
