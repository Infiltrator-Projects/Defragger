# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the authoritative native C XFS engine.

"""XFS plugin facade.

All XFS on-disk parsing, allocation analysis, planning, relocation, metadata
rewriting and verification live in ``native/`` C sources owned by this plugin.
Python is deliberately limited to the stable GUI/backend contract and map-cell
aggregation.
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
    "xfs",
    "XFS",
    ("xfs",),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation(
            "defrag",
            "xfs-native",
            warning=(
                "XFS writing uses Linux Defragger's offline native C raw engine. "
                "No XFS kernel driver, mount, filesystem ioctl or external repair tool is used."
            ),
        ),
        operation(
            "growth-defrag",
            "xfs-native",
            warning=(
                "XFS Growth Defrag uses the native C raw engine, requires staging space, "
                "and preserves an exact 10% free run after every supported regular file."
            ),
        ),
        operation("recover", "xfs-native"),
    ),
)


class XfsBackend(FilesystemBackend):
    """Expose the native C XFS engine through the standard GUI plugin ABI."""

    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("xfs-native", anchor=anchor)
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
            raise BackendError(f"native XFS worker returned invalid JSON: {exc}") from exc
        if not isinstance(result, dict):
            raise BackendError("native XFS worker returned a non-object result")
        return result

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
        except (FileNotFoundError, OSError):
            return False
        if completed.returncode != 0:
            return False
        payload = self._json_result(completed)
        return payload.get("filesystem") == "xfs"

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "analyse-json")
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native XFS analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "xfs":
            raise BackendError("native XFS analyser returned the wrong filesystem identity")

        try:
            block_size = int(payload["block_size"])
            total_blocks = int(payload["dblocks"])
            free_ranges = [
                (int(item[0]), int(item[1]))
                for item in payload["free_ranges"]
                if isinstance(item, list) and len(item) == 2
            ]
        except (KeyError, TypeError, ValueError) as exc:
            raise BackendError("native XFS analyser returned incomplete geometry") from exc

        if block_size <= 0 or total_blocks <= 0:
            raise BackendError("native XFS analyser returned invalid geometry")

        used_ranges = complement_ranges(total_blocks, free_ranges)
        state_ranges = [(start, end, 0) for start, end in free_ranges]
        state_ranges.extend((start, end, 1) for start, end in used_ranges)
        details = {
            "block_size": block_size,
            "sector_size": int(payload.get("sector_size", 0)),
            "inode_size": int(payload.get("inode_size", 0)),
            "allocation_group_blocks": int(payload.get("agblocks", 0)),
            "allocation_groups": payload.get("allocation_groups", []),
            "bnobt_blocks": int(payload.get("bnobt_blocks", 0)),
            "superblock_free_blocks": int(payload.get("fdblocks", 0)),
            "free_space_basis": "XFS per-allocation-group block-number free-space B+trees",
            "fragmentation_available": True,
            "fragmentation_basis": "native C XFS inode B+tree and data-fork extent scanner",
            "inodes_scanned": int(payload.get("inodes_scanned", 0)),
            "malformed_inodes": int(payload.get("malformed_inodes", 0)),
            "realtime_inodes_not_mapped": int(payload.get("realtime_inodes", 0)),
            "inobt_blocks": int(payload.get("inobt_blocks", 0)),
            "bmap_blocks": int(payload.get("bmap_blocks", 0)),
        }
        result = aggregate_ranges(
            total_blocks,
            max(1, cells),
            block_size,
            "xfs",
            state_ranges,
            "exact",
            details,
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
                "fragmentation_percent": float(payload.get("fragmentation_percent", 0.0)),
            }
        )
        result["details"].update(
            {
                "fragmented_blocks_mapped": fragmented_blocks,
                "directory_blocks_mapped": directory_blocks,
            }
        )
        return result


BACKEND = XfsBackend()
