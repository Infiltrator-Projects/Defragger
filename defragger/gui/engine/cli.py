#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Command-line entry point for the shared operation engine."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from backends import BackendError, Registry
from core.devices import require_unmounted
from version import VERSION

from .dispatch import build_worker_command

HERE = Path(__file__).resolve().parents[1]


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description="Dispatch a Linux Defragger operation through a filesystem plugin"
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {VERSION}")
    parser.add_argument("--list-plugins", action="store_true")
    parser.add_argument("operation", nargs="?")
    parser.add_argument("device", nargs="?")
    parser.add_argument("--filesystem", default="")
    return parser.parse_known_args(argv)


def main(argv: list[str] | None = None) -> int:
    args, forwarded = parse_args(sys.argv[1:] if argv is None else argv)
    registry = Registry()
    if args.list_plugins:
        print(json.dumps({"schema": 3, "backends": registry.manifest()}, separators=(",", ":")))
        return 0
    if not args.operation or not args.device or not args.filesystem:
        print("operation, device and --filesystem are required", file=sys.stderr)
        return 2
    try:
        require_unmounted(args.device)
        command = build_worker_command(
            registry,
            args.filesystem,
            args.operation,
            args.device,
            forwarded,
            anchor=HERE / "core",
        )
    except (BackendError, FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"linux-defragger-operation-engine: {exc}", file=sys.stderr)
        return 2
    os.execv(command[0], command)
    return 127
