# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Shared FAT12/16/32 plugin declaration and signature probe.

"""Standard FAT plugin shared by the three on-disk FAT widths."""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from core.paths import resolve_program

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
    operation,
)


@dataclass(frozen=True, slots=True)
class FatIdentity:
    """Validated result from the native authoritative FAT geometry parser."""

    filesystem: str
    bits: int

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "FatIdentity":
        filesystem = payload.get("filesystem")
        bits = payload.get("bits")
        if not isinstance(filesystem, str) or bits not in (12, 16, 32):
            raise BackendError("native FAT identifier returned an invalid result")
        if filesystem != f"FAT{bits}":
            raise BackendError("native FAT identifier returned inconsistent width data")
        return cls(filesystem=filesystem, bits=bits)


class FatBackend(FilesystemBackend):
    def __init__(self, fat_bits: int):
        self.fat_bits = fat_bits
        self.info = BackendInfo(
            id=f"fat{fat_bits}",
            display_name=f"FAT{fat_bits}",
            aliases=(f"fat{fat_bits}", "vfat" if fat_bits == 32 else f"msdos{fat_bits}"),
            capabilities=(
                CAP_ANALYSE
                | CAP_MAP
                | CAP_DEFRAG
                | CAP_RECOVER
                | CAP_LIVE_MAP
                | CAP_GROWTH_DEFRAG
            ),
            map_accuracy="exact",
            operations=(
                operation("defrag", "fat-native"),
                operation("growth-defrag", "fat-native"),
                operation("recover", "fat-native"),
            ),
        )

    @staticmethod
    def _run_native(
        path: str,
        operation: str,
        *options: str,
    ) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("fat-native", anchor=anchor)
        return subprocess.run(
            [worker, operation, path, *options],
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
            raise BackendError(f"native FAT worker returned invalid JSON: {exc}") from exc
        if not isinstance(result, dict):
            raise BackendError("native FAT worker returned a non-object result")
        return result

    def probe(self, path: str) -> bool:
        completed = self._run_native(path, "identify")
        if completed.returncode != 0:
            return False
        return FatIdentity.from_payload(self._json_result(completed)).bits == self.fat_bits

    def map(self, path: str, cells: int) -> dict:
        """Delegate the mature FAT scanner through the standard plugin method."""

        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native FAT mapper failed")
        return self._json_result(completed)


__all__ = ["FatBackend", "FatIdentity"]
