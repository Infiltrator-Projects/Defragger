# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the authoritative native C EXT2/3/4 engine.

"""EXT2/3/4 plugin facade.

All filesystem parsing, planning, relocation and verification live in the
plugin-owned ``native/`` C engine.  Python only adapts its stable JSON output to
the GUI map contract.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import (
    BackendError,
    BackendInfo,
    CAP_ANALYSE,
    CAP_DEFRAG,
    CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP,
    CAP_MAP,
    CAP_RECOVER,
    FilesystemBackend,
    aggregate_ranges,
    complement_ranges,
    overlay_ranges,
    operation,
)
from core.paths import resolve_program


INFO = BackendInfo(
    "ext4",
    "ext2/3/4",
    ("ext2", "ext3", "ext4"),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation("defrag", "ext-native"),
        operation("growth-defrag", "ext-native"),
        operation("recover", "ext-native"),
    ),
)


class ExtBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("ext-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )

    @staticmethod
    def _json_result(completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        try:
            result = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip()
            if detail:
                raise BackendError(detail) from exc
            raise BackendError(f"native EXT worker returned invalid JSON: {exc}") from exc
        if not isinstance(result, dict):
            raise BackendError("native EXT worker returned a non-object result")
        return result

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
        except (FileNotFoundError, OSError):
            return False
        if completed.returncode != 0:
            return False
        payload = self._json_result(completed)
        return payload.get("filesystem") in {"ext2", "ext3", "ext4"}

    def map(self, path: str, cells: int) -> dict[str, object]:
        completed = self._run_native(path, "analyse-json")
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native EXT analyser failed")
        payload = self._json_result(completed)
        filesystem = str(payload.get("filesystem", ""))
        if filesystem not in {"ext2", "ext3", "ext4"}:
            raise BackendError("native EXT analyser returned the wrong filesystem identity")
        try:
            block_size = int(payload["block_size"])
            total_blocks = int(payload["total_blocks"])
            free_ranges = [
                (int(item[0]), int(item[1]))
                for item in payload["free_ranges"]
                if isinstance(item, list) and len(item) == 2
            ]
        except (KeyError, TypeError, ValueError) as exc:
            raise BackendError("native EXT analyser returned incomplete geometry") from exc
        if block_size <= 0 or total_blocks <= 0:
            raise BackendError("native EXT analyser returned invalid geometry")

        used_ranges = complement_ranges(total_blocks, free_ranges)
        states = [(start, end, 0) for start, end in free_ranges]
        states.extend((start, end, 1) for start, end in used_ranges)
        result = aggregate_ranges(
            total_blocks,
            max(1, cells),
            block_size,
            filesystem,
            states,
            "exact",
            {
                "fragmentation_available": True,
                "fragmentation_basis": "native C EXT inode allocation scanner",
                "inodes_scanned": int(payload.get("inodes_scanned", 0)),
                "malformed_inodes": int(payload.get("malformed_inodes", 0)),
                "growth_10_satisfied": bool(payload.get("growth_10_satisfied", False)),
            },
        )
        fragmented_ranges = [
            (int(item[0]), int(item[1]))
            for item in payload.get("fragmented_ranges", [])
            if isinstance(item, list) and len(item) == 2
        ]
        directory_ranges = [
            (int(item[0]), int(item[1]))
            for item in payload.get("directory_ranges", [])
            if isinstance(item, list) and len(item) == 2
        ]
        fragmented_blocks = overlay_ranges(result["cells"], fragmented_ranges, "fragmented")
        directory_blocks = overlay_ranges(result["cells"], directory_ranges, "directory")
        result.update(
            {
                "regular_files": int(payload.get("regular_files", 0)),
                "directories": int(payload.get("directories", 0)),
                "fragmented_files": int(payload.get("fragmented_files", 0)),
                "fragmented_directories": int(payload.get("fragmented_directories", 0)),
                "fragmentation_percent": (
                    100.0
                    * int(payload.get("fragmented_files", 0))
                    / max(1, int(payload.get("regular_files", 0)))
                ),
            }
        )
        result["details"].update(
            {
                "fragmented_blocks_mapped": fragmented_blocks,
                "directory_blocks_mapped": directory_blocks,
            }
        )
        return result


BACKEND = ExtBackend()
