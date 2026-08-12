# SPDX-License-Identifier: GPL-3.0-or-later
"""Typed, filesystem-neutral worker event protocol."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

_EVENT_PREFIXES = {
    "@@PHASE ": "phase",
    "@@LIVE_RESET ": "live-reset",
    "@@LIVE_RANGE ": "live-range",
    "@@LIVE_RANGES ": "live-ranges",
    "@@LIVE_MAP ": "live-map",
    "@@RESULT ": "result",
}
_OPERATIONS = {"defrag", "growth-defrag", "recover"}
_RESULT_STATUSES = {"completed", "not-needed", "stopped", "failed"}


@dataclass(frozen=True, slots=True)
class OperationResult:
    """Machine-readable semantic result emitted by a mutation worker."""

    operation: str
    status: str
    message: str = ""

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "OperationResult":
        operation = str(payload.get("operation", ""))
        status = str(payload.get("status", ""))
        message = str(payload.get("message", ""))
        if operation not in _OPERATIONS:
            raise ValueError(f"result event has invalid operation: {operation!r}")
        if status not in _RESULT_STATUSES:
            raise ValueError(f"result event has invalid status: {status!r}")
        return cls(operation=operation, status=status, message=message)


@dataclass(frozen=True, slots=True)
class EngineEvent:
    kind: str
    payload: dict[str, Any]

    def operation_result(self) -> OperationResult:
        if self.kind != "result":
            raise ValueError(f"{self.kind!r} is not an operation-result event")
        return OperationResult.from_payload(self.payload)


class EngineEventParser:
    """Parse one worker stdout line without GTK or filesystem knowledge."""

    @staticmethod
    def is_event_line(line: str) -> bool:
        return any(line.startswith(prefix) for prefix in _EVENT_PREFIXES)

    @classmethod
    def parse(cls, line: str) -> EngineEvent | None:
        for prefix, kind in _EVENT_PREFIXES.items():
            if not line.startswith(prefix):
                continue
            payload = json.loads(line[len(prefix):])
            if not isinstance(payload, dict):
                raise ValueError(f"{kind} event payload is not an object")
            event = EngineEvent(kind=kind, payload=payload)
            if kind == "result":
                event.operation_result()
            return event
        return None


def emit_operation_result(operation: str, status: str, message: str = "") -> None:
    """Emit a validated result event from a Python filesystem worker."""

    result = OperationResult(operation=operation, status=status, message=message)
    # Reuse the parser's validation so producers and consumers accept the same values.
    OperationResult.from_payload(
        {"operation": result.operation, "status": result.status, "message": result.message}
    )
    payload = {
        "operation": result.operation,
        "status": result.status,
        "message": result.message,
    }
    print("@@RESULT " + json.dumps(payload, separators=(",", ":")), flush=True)
