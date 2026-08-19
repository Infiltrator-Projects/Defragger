# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for native C Linux swap identification and mapping.

"""Read-only Linux swap backend delegated entirely to the native C worker."""

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
    CAP_MAP,
    FilesystemBackend,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "swap",
    "Linux Swap",
    ("swap", "swapspace", "linux-swap"),
    CAP_ANALYSE | CAP_MAP,
    "summary",
)


class SwapBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(
        path: str,
        mode: str,
        *options: str,
    ) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("swap-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *options],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )

    @staticmethod
    def _json_result(completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip()
            if detail:
                raise BackendError(detail) from exc
            raise BackendError("native Swap worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native Swap worker returned a non-object result")
        if payload.get("filesystem") != "swap":
            raise BackendError("native Swap worker returned the wrong filesystem identity")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed)["filesystem"] == "swap"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native Swap mapper failed")
        payload = self._json_result(completed)
        if payload.get("schema") != 1 or payload.get("map_accuracy") not in {"exact", "summary"}:
            raise BackendError("native Swap mapper returned an invalid map contract")
        return payload


BACKEND = SwapBackend()
