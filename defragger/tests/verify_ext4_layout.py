#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independently verify EXT4 contiguity and the packed-prefix map invariant."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"EXT4 verification failed: {message}")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} MAPPER IMAGE MODE")
    mapper = Path(sys.argv[1])
    image = Path(sys.argv[2])
    mode = sys.argv[3]
    if mode not in {"defrag", "growth"}:
        fail(f"unsupported mode {mode!r}")

    # One map cell per physical filesystem block makes the free-space test exact.
    probe = json.loads(
        subprocess.check_output(
            [sys.executable, str(mapper), str(image), "--fstype", "ext4", "--cells", "131072"],
            text=True,
        )
    )
    if probe["fragmented_files"] != 0:
        fail(f"{probe['fragmented_files']} fragmented files remain")
    if probe["fragmented_directories"] != 0:
        fail(f"{probe['fragmented_directories']} fragmented directories remain")
    if probe["cell_count"] != probe["total_units"]:
        fail("test map is not block-exact")

    cells = probe["cells"]
    allocated = [
        index
        for index, cell in enumerate(cells)
        if cell.get("used", 0)
        or cell.get("fragmented", 0)
        or cell.get("directory", 0)
        or cell.get("unknown", 0)
        or cell.get("bad", 0)
    ]
    if not allocated:
        fail("no allocated blocks were reported")
    highest = max(allocated)
    free_below = [index for index, cell in enumerate(cells[: highest + 1]) if cell.get("free", 0)]
    if mode == "defrag" and free_below:
        fail(f"free block {free_below[0]} remains below the final allocation")
    if mode == "growth" and not free_below:
        fail("Growth Defrag created no post-file reserves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
