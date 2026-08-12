# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Discover and validate filesystem plugin packages.

"""Filesystem plugin discovery.

Each filesystem lives in ``filesystems/<id>/`` and exposes ``BACKEND`` from
``plugin.py``.  The registry contains no filesystem-specific import list, so a
new plugin is added by adding a package rather than editing the engine.
"""

from __future__ import annotations

import importlib
import pkgutil
from collections.abc import Iterable

from .base import BackendError, FilesystemBackend


def discover_plugin_names() -> tuple[str, ...]:
    """Return installed filesystem plugin package names in stable order."""

    package = importlib.import_module("filesystems")
    names = [
        item.name
        for item in pkgutil.iter_modules(package.__path__)
        if item.ispkg and item.name != "fat"
    ]
    return tuple(sorted(names))


class Registry:
    """Ordered plugin registry with strict ID and alias validation."""

    def __init__(self, module_names: Iterable[str] | None = None):
        self.backends: list[FilesystemBackend] = []
        self.aliases: dict[str, FilesystemBackend] = {}
        self.ids: dict[str, FilesystemBackend] = {}
        for name in module_names or discover_plugin_names():
            module = importlib.import_module(f"filesystems.{name}.plugin")
            backend = getattr(module, "BACKEND", None)
            if not isinstance(backend, FilesystemBackend):
                raise BackendError(
                    f"plugin package {name!r} does not expose a FilesystemBackend as BACKEND"
                )
            self._register(backend)

    def _register(self, backend: FilesystemBackend) -> None:
        backend_id = backend.info.id.lower()
        if backend_id in self.ids:
            raise BackendError(f"duplicate filesystem plugin id: {backend_id}")
        self.ids[backend_id] = backend
        self.backends.append(backend)
        for alias in (backend_id, *backend.info.aliases):
            key = alias.lower()
            previous = self.aliases.get(key)
            if previous is not None and previous is not backend:
                raise BackendError(
                    f"filesystem alias {key!r} is declared by both "
                    f"{previous.info.id} and {backend.info.id}"
                )
            self.aliases[key] = backend

    def by_fstype(self, fstype: str) -> FilesystemBackend | None:
        return self.aliases.get(fstype.lower())

    def probe(self, path: str) -> FilesystemBackend | None:
        for backend in self.backends:
            try:
                if backend.probe(path):
                    return backend
            except (OSError, BackendError):
                continue
        return None

    def manifest(self) -> list[dict[str, object]]:
        return [backend.info.manifest() for backend in self.backends]
