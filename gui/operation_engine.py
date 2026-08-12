#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility launcher for the shared operation engine package."""

from core.paths import resolve_program
from engine.cli import main, parse_args
from engine.dispatch import build_worker_command as _build_worker_command
from engine.options import without_options as _without_options


def build_worker_command(registry, filesystem, operation_name, device, forwarded):
    """Compatibility facade retaining the historical resolver injection point."""

    return _build_worker_command(
        registry,
        filesystem,
        operation_name,
        device,
        forwarded,
        resolver=resolve_program,
    )


__all__ = ["build_worker_command", "main", "parse_args", "resolve_program"]

if __name__ == "__main__":
    raise SystemExit(main())
