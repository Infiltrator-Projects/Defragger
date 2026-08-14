#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Thin GUI adapter for native C APFS container identification and geometry.

"""APFS container summary backend.

APFS allocation ownership is checkpointed through spaceman objects and can be
shared by multiple volumes. The native C worker owns on-disk NX superblock
parsing. Python only adapts its summary to the stable GUI map contract.
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
    CAP_MAP,
    FilesystemBackend,
    aggregate_ranges,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "apfs",
    "Apple APFS",
    ("apfs",),
    CAP_ANALYSE | CAP_MAP,
    "summary",
)


class APFSBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("apfs-native", anchor=anchor)
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
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip()
            if detail:
                raise BackendError(detail) from exc
            raise BackendError("native APFS worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native APFS worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return (
                completed.returncode == 0
                and self._json_result(completed).get("filesystem") == "apfs"
            )
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "analyse-json")
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native APFS analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "apfs":
            raise BackendError("native APFS analyser returned the wrong filesystem identity")
        try:
            block_size = int(payload["block_size"])
            block_count = int(payload["block_count"])
            container_uuid = str(payload["container_uuid"])
        except (KeyError, TypeError, ValueError) as exc:
            raise BackendError("native APFS analyser returned incomplete geometry") from exc
        if block_size < 4096 or block_size & (block_size - 1) or block_count <= 0:
            raise BackendError("native APFS analyser returned invalid geometry")
        if len(container_uuid) != 32:
            raise BackendError("native APFS analyser returned an invalid container UUID")

        return aggregate_ranges(
            block_count,
            max(1, cells),
            block_size,
            "apfs",
            [(0, 1, 1), (1, block_count, 2)],
            "summary",
            details={
                "container_uuid": container_uuid,
                "note": "APFS spaceman allocation map not yet decoded",
            },
        )


BACKEND = APFSBackend()
