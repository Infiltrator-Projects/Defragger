# SPDX-License-Identifier: GPL-3.0-or-later
"""Immutable GUI view of the validated filesystem backend manifest."""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping


@dataclass(frozen=True, slots=True)
class BackendCatalog:
    """Backend aliases, capabilities and operations owned by one GUI instance."""

    aliases: Mapping[str, str]
    capabilities: Mapping[str, int]
    operations: Mapping[str, Mapping[str, Mapping[str, Any]]]

    @classmethod
    def from_manifest(cls, data: dict[str, Any]) -> "BackendCatalog":
        entries = data.get("backends")
        if not isinstance(entries, list):
            raise ValueError("backend manifest has no backends list")

        aliases: dict[str, str] = {}
        capabilities: dict[str, int] = {}
        operations: dict[str, Mapping[str, Mapping[str, Any]]] = {}
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError("backend manifest entry is not an object")
            backend_id = str(entry.get("id", "")).lower()
            if not backend_id or backend_id in capabilities:
                raise ValueError(f"invalid or duplicate backend id: {backend_id!r}")
            capabilities[backend_id] = int(entry.get("capabilities", 0))

            operation_table: dict[str, Mapping[str, Any]] = {}
            declared_operations = entry.get("operations", [])
            if not isinstance(declared_operations, list):
                raise ValueError(f"backend {backend_id!r} operations are not a list")
            for item in declared_operations:
                if not isinstance(item, dict):
                    raise ValueError(f"backend {backend_id!r} operation is not an object")
                name = str(item.get("name", ""))
                if not name or name in operation_table:
                    raise ValueError(
                        f"backend {backend_id!r} has an invalid or duplicate operation {name!r}"
                    )
                operation_table[name] = MappingProxyType(dict(item))
            operations[backend_id] = MappingProxyType(operation_table)

            declared_aliases = entry.get("aliases", [])
            if not isinstance(declared_aliases, list):
                raise ValueError(f"backend {backend_id!r} aliases are not a list")
            for alias in (backend_id, *declared_aliases):
                key = str(alias).lower()
                previous = aliases.get(key)
                if not key or (previous is not None and previous != backend_id):
                    raise ValueError(f"backend alias {key!r} is ambiguous")
                aliases[key] = backend_id

        # Linux reports all classic FAT variants as vfat on ordinary mounts.
        for alias in ("vfat", "fat", "msdos"):
            aliases.setdefault(alias, "fat32")

        return cls(
            aliases=MappingProxyType(aliases),
            capabilities=MappingProxyType(capabilities),
            operations=MappingProxyType(operations),
        )

    def supports(self, fstype: str) -> bool:
        return fstype.lower() in self.aliases

    def normalize(self, fstype: str) -> str:
        value = fstype.lower()
        return self.aliases.get(value, value)

    def capabilities_for(self, fstype: str) -> int:
        return self.capabilities.get(self.normalize(fstype), 0)

    def operations_for(self, fstype: str) -> Mapping[str, Mapping[str, Any]]:
        operations = self.operations.get(self.normalize(fstype))
        return operations if operations is not None else MappingProxyType({})
