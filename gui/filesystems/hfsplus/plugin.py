#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the authoritative native C HFS+/HFSX engine.

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import (
    BackendError, BackendInfo, CAP_ANALYSE, CAP_DEFRAG, CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP, CAP_MAP, CAP_RECOVER, FilesystemBackend, aggregate_ranges,
    complement_ranges, overlay_ranges, operation,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "hfsplus", "Apple HFS+/HFSX", ("hfsplus", "hfs+", "hfsx"),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation("defrag", "hfsplus-native", warning="HFS+/HFSX writing uses Linux Defragger's offline first-party native C raw engine."),
        operation("growth-defrag", "hfsplus-native", warning="HFS+/HFSX Growth Defrag uses the native C raw engine and leaves an exact 10% free allocation-block reserve after each movable fork."),
        operation("recover", "hfsplus-native"),
    ),
)


class HFSPlusBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run(path: str, mode: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("hfsplus-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path], check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )

    @staticmethod
    def _json(completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        try:
            value = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            raise BackendError(completed.stderr.strip() or "native HFS+ worker returned invalid JSON") from exc
        if not isinstance(value, dict):
            raise BackendError("native HFS+ worker returned a non-object result")
        return value

    def probe(self, path: str) -> bool:
        try:
            completed = self._run(path, "identify")
        except (FileNotFoundError, OSError):
            return False
        return completed.returncode == 0 and self._json(completed).get("filesystem") == "hfsplus"

    def map(self, path: str, cells: int) -> dict:
        completed = self._run(path, "analyse-json")
        if completed.returncode != 0:
            raise BackendError(completed.stderr.strip() or "native HFS+ analyser failed")
        payload = self._json(completed)
        total = int(payload["total_blocks"])
        block_size = int(payload["block_size"])
        free_ranges = [(int(a), int(b)) for a, b in payload.get("free_ranges", [])]
        used = complement_ranges(total, free_ranges)
        state = [(a, b, 0) for a, b in free_ranges] + [(a, b, 1) for a, b in used]
        result = aggregate_ranges(
            total, max(1, cells), block_size,
            "hfsx" if payload.get("signature") == "HX" else "hfsplus",
            state, "exact", {
                "block_size": block_size,
                "variant": payload.get("variant", "HFS+"),
                "journaled": bool(payload.get("journaled", False)),
                "fragmentation_available": True,
                "fragmentation_basis": "first-party native C HFS+/HFSX catalog and allocation-file scan",
            },
        )
        fragmented = [(int(a), int(b)) for a, b in payload.get("fragmented_ranges", [])]
        directories = [(int(a), int(b)) for a, b in payload.get("directory_ranges", [])]
        result.update({
            "regular_files": int(payload.get("regular_files", 0)),
            "directories": int(payload.get("directories", 0)),
            "fragmented_files": int(payload.get("fragmented_files", 0)),
            "fragmented_directories": int(payload.get("fragmented_directories", 0)),
        })
        result["details"].update({
            "header_free_blocks": int(payload.get("free_blocks", 0)),
            "fragmented_blocks_mapped": overlay_ranges(result["cells"], fragmented, "fragmented"),
            "directory_blocks_mapped": overlay_ranges(result["cells"], directories, "directory"),
        })
        return result


BACKEND = HFSPlusBackend()
