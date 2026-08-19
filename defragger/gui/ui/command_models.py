# SPDX-License-Identifier: GPL-3.0-or-later
"""Typed contracts shared by command execution and privilege IPC services."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Literal


EventKind = Literal[
    "engine",
    "error",
    "helper-closed",
    "helper-ready",
    "helper-starting",
    "output",
    "progress",
    "started",
    "stop-delivered",
    "stop-failed",
    "stop-requested",
]


@dataclass(frozen=True, slots=True)
class RunnerEvent:
    """One presentation-neutral event emitted by a command service."""

    kind: EventKind
    purpose: str = ""
    message: str = ""
    percent: float | None = None


@dataclass(frozen=True, slots=True)
class CommandCompletion:
    """Final process result delivered exactly once for a command request."""

    returncode: int
    output: str
    purpose: str


CompletionCallback = Callable[[CommandCompletion], None]
EventCallback = Callable[[RunnerEvent], None]
Scheduler = Callable[..., Any]


@dataclass(frozen=True, slots=True)
class CommandRequest:
    """Immutable request accepted by the one-command-at-a-time runner."""

    arguments: tuple[str, ...]
    purpose: str
    privileged: bool
    stream_output: bool
    on_complete: CompletionCallback
