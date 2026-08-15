#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify that red map units identify displaced extents, not a whole file."""

from __future__ import annotations

import json
import sys
from pathlib import Path


data = json.loads(Path(sys.argv[1]).read_text())
cells = {int(cell["start"]): cell for cell in data["cells"]}

assert data["fragmented_files"] == 1
assert cells[5]["used"] == 1 and cells[5]["fragmented"] == 0
assert cells[10]["used"] == 1 and cells[10]["fragmented"] == 1
assert cells[6]["used"] == 1 and cells[6]["fragmented"] == 1
assert sum(int(cell["fragmented"]) for cell in data["cells"]) == 2

print("verified map red marks only the two displaced FAT extents")
