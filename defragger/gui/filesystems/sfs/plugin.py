#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the native C Amiga Smart File System analyser.

"""Read-only SFS0 allocation backend.

The native worker validates redundant root blocks and every BTMP allocation
bitmap block directly from raw storage.  SFS2 is intentionally not advertised as supported by this backend until its
large-file variant has independent fixtures and an on-disk compatibility audit.
The SFS0 decoder therefore exposes no ``sfs2`` alias.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import BackendError, BackendInfo, CAP_ANALYSE, CAP_MAP, FilesystemBackend
from core.paths import resolve_program

INFO = BackendInfo(
    "sfs", "Amiga SFS", ("sfs", "sfs0", "smartfilesystem"),
    CAP_ANALYSE | CAP_MAP,
    "exact-allocation",
)


class SfsBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str, *args: object) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("sfs-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *map(str, args)],
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
            raise BackendError("native SFS worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native SFS worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return (
                completed.returncode == 0
                and self._json_result(completed).get("filesystem") == "sfs"
            )
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", max(1, int(cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native SFS analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "sfs":
            raise BackendError("native SFS analyser returned the wrong filesystem identity")
        if payload.get("schema") != 1 or payload.get("backend") != "read-only-domain":
            raise BackendError("native SFS analyser returned an incompatible map schema")
        if payload.get("map_accuracy") != "exact-allocation":
            raise BackendError("native SFS analyser returned an unexpected accuracy contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native SFS analyser returned an incomplete map")
        return payload


BACKEND = SfsBackend()
