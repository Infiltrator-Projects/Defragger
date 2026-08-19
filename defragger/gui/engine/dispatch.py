# SPDX-License-Identifier: GPL-3.0-or-later
"""Filesystem-neutral worker dispatch."""

from __future__ import annotations

from pathlib import Path
from collections.abc import Callable

from backends import BackendError, Registry
from core.paths import resolve_program

from .options import without_options


def build_worker_command(
    registry: Registry,
    filesystem: str,
    operation_name: str,
    device: str,
    forwarded: list[str],
    *,
    anchor: Path | None = None,
    resolver: Callable[..., str] = resolve_program,
) -> list[str]:
    """Build the command declared by a filesystem plugin manifest."""

    backend = registry.by_fstype(filesystem)
    if backend is None:
        raise BackendError(f"no filesystem plugin is registered for {filesystem!r}")
    specification = backend.info.operation(operation_name)
    if specification is None:
        raise BackendError(
            f"the {backend.info.display_name} plugin does not implement {operation_name}"
        )
    worker = resolver(specification.worker, anchor=anchor)
    arguments = without_options(forwarded, specification.unsupported_options)
    return [worker, operation_name, device, *arguments]
