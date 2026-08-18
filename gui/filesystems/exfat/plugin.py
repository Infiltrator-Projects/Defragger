# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the authoritative native C exFAT engine.

"""exFAT plugin facade.

All exFAT on-disk parsing, allocation planning, directory-entry rewriting,
canonical dependency-aware relayout, verification, journalling and recovery live in ``native/`` C sources
owned by this plugin. Python only adapts the native worker's JSON to the
stable GUI/backend map contract.
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
    "exfat",
    "exFAT",
    ("exfat",),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation(
            "defrag",
            "exfat-native",
            warning=(
                "exFAT writing uses Linux Defragger's offline native C raw engine. "
                "No exFAT filesystem driver, mount, filesystem ioctl or external "
                "filesystem utility is used."
            ),
        ),
        operation(
            "growth-defrag",
            "exfat-native",
            warning=(
                "exFAT Growth Defrag uses the same native C raw engine and leaves "
                "an exact 10% free-cluster reserve after every regular file."
            ),
        ),
        operation("recover", "exfat-native"),
    ),
)


class ExfatBackend(FilesystemBackend):
    """Expose the native C exFAT engine through the standard GUI plugin ABI."""

    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("exfat-native", anchor=anchor)
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
            raise BackendError(f"native exFAT worker returned invalid JSON: {exc}") from exc
        if not isinstance(result, dict):
            raise BackendError("native exFAT worker returned a non-object result")
        return result

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
        except (FileNotFoundError, OSError):
            return False
        return completed.returncode == 0 and self._json_result(completed).get("filesystem") == "exfat"

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "analyse-json")
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native exFAT analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "exfat":
            raise BackendError("native exFAT analyser returned the wrong filesystem identity")
        try:
            cluster_size = int(payload["cluster_size"])
            total_clusters = int(payload["total_clusters"])
            free_ranges = [
                (int(item[0]), int(item[1]))
                for item in payload["free_ranges"]
                if isinstance(item, list) and len(item) == 2
            ]
        except (KeyError, TypeError, ValueError) as exc:
            raise BackendError("native exFAT analyser returned incomplete geometry") from exc
        if cluster_size <= 0 or total_clusters <= 0:
            raise BackendError("native exFAT analyser returned invalid geometry")

        used_ranges = complement_ranges(total_clusters, free_ranges)
        state_ranges = [(start, end, 0) for start, end in free_ranges]
        state_ranges.extend((start, end, 1) for start, end in used_ranges)
        result = aggregate_ranges(
            total_clusters,
            max(1, cells),
            cluster_size,
            "exfat",
            state_ranges,
            "exact",
            {
                "cluster_size": cluster_size,
                "serial": payload.get("serial", ""),
                "fragmentation_available": True,
                "fragmentation_basis": "native C exFAT FAT/bitmap/directory catalogue",
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
        fragmented_clusters = overlay_ranges(result["cells"], fragmented_ranges, "fragmented")
        directory_clusters = overlay_ranges(result["cells"], directory_ranges, "directory")
        result.update(
            {
                "regular_files": int(payload.get("regular_files", 0)),
                "directories": int(payload.get("directories", 0)),
                "fragmented_files": int(payload.get("fragmented_files", 0)),
                "fragmented_directories": int(payload.get("fragmented_directories", 0)),
                "fragmentation_percent": float(payload.get("fragmentation_percent", 0.0)),
                "growth_10_satisfied": bool(payload.get("growth_10_satisfied", False)),
            }
        )
        result["details"].update(
            {
                "fragmented_clusters_mapped": fragmented_clusters,
                "directory_clusters_mapped": directory_clusters,
            }
        )
        return result


BACKEND = ExfatBackend()
