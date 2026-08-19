#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stable read-only and raw-offline mutation contracts for filesystem plugins."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import IntFlag

__all__ = [
    "BackendError",
    "BackendInfo",
    "Capability",
    "FilesystemBackend",
    "OperationSpec",
    "CAP_ANALYSE",
    "CAP_DEFRAG",
    "CAP_GROWTH_DEFRAG",
    "CAP_LIVE_MAP",
    "CAP_MAP",
    "CAP_RECOVER",
    "operation",
]


class Capability(IntFlag):
    """Plugin capabilities exposed to the GUI.

    There is deliberately no separate free-space operation. Defragment is the only normal
    layout operation and means both complete fragmentation removal and complete
    physical packing of the filesystem's allocations.
    """

    ANALYSE = 1 << 0
    MAP = 1 << 1
    DEFRAG = 1 << 2
    RECOVER = 1 << 3
    LIVE_MAP = 1 << 4
    GROWTH_DEFRAG = 1 << 5


CAP_ANALYSE = int(Capability.ANALYSE)
CAP_MAP = int(Capability.MAP)
CAP_DEFRAG = int(Capability.DEFRAG)
CAP_RECOVER = int(Capability.RECOVER)
CAP_LIVE_MAP = int(Capability.LIVE_MAP)
CAP_GROWTH_DEFRAG = int(Capability.GROWTH_DEFRAG)

_OPERATION_CAPABILITY = {
    "defrag": Capability.DEFRAG,
    "growth-defrag": Capability.GROWTH_DEFRAG,
    "recover": Capability.RECOVER,
}

_STANDARD_LABELS = {
    "defrag": "Defragment",
    "growth-defrag": "Growth Defrag",
    "recover": "Recover",
}

_STANDARD_DESCRIPTIONS = {
    "defrag": (
        "Rewrite the unmounted filesystem directly so files and directories are contiguous "
        "and every relocatable allocation is packed into the earliest legal allocation units."
    ),
    "growth-defrag": (
        "Apply the same packed contiguous layout while reserving 10% of each regular file's "
        "allocated length immediately after that file."
    ),
    "recover": "Complete or roll back an interrupted raw-device journal transaction.",
}

_LAYOUT_POLICIES = {"packed", "growth-10", "recovery"}


class BackendError(RuntimeError):
    """A filesystem plugin could not safely identify, analyse or mutate a volume."""


@dataclass(frozen=True, slots=True)
class OperationSpec:
    """One direct, offline mutation supplied by a filesystem plugin.

    A plugin may only declare a write operation when its worker opens the
    unmounted device or image directly. Mounted-filesystem APIs, private mounts
    and filesystem relocation ioctls are not permitted. A format utility may
    inspect or repair only a private persistent stage while the source remains
    unchanged; it may never authoritatively mutate the source target.
    """

    name: str
    worker: str
    layout_policy: str
    label: str = ""
    description: str = ""
    warning: str = ""
    unsupported_options: tuple[str, ...] = ()
    raw_offline: bool = True
    live_updates: bool = True

    def __post_init__(self) -> None:
        if self.name not in _OPERATION_CAPABILITY:
            raise ValueError(f"unknown filesystem operation: {self.name}")
        if not self.worker or any(char.isspace() for char in self.worker):
            raise ValueError(f"invalid worker identifier: {self.worker!r}")
        if self.layout_policy not in _LAYOUT_POLICIES:
            raise ValueError(f"invalid layout policy: {self.layout_policy!r}")
        expected = {
            "defrag": "packed",
            "growth-defrag": "growth-10",
            "recover": "recovery",
        }[self.name]
        if self.layout_policy != expected:
            raise ValueError(
                f"operation {self.name} must use layout policy {expected!r}, not {self.layout_policy!r}"
            )
        if not self.raw_offline:
            raise ValueError(f"operation {self.name} must be a direct raw-offline writer")
        if self.name != "recover" and not self.live_updates:
            raise ValueError(f"operation {self.name} must publish live allocation-map updates")
        if any(not option.startswith("--") for option in self.unsupported_options):
            raise ValueError("unsupported options must use their long --option form")

    @property
    def capability(self) -> Capability:
        return _OPERATION_CAPABILITY[self.name]

    def manifest(self) -> dict[str, object]:
        return {
            "name": self.name,
            "worker": self.worker,
            "layout_policy": self.layout_policy,
            "label": self.label or _STANDARD_LABELS[self.name],
            "description": self.description or _STANDARD_DESCRIPTIONS[self.name],
            "warning": self.warning,
            "unsupported_options": list(self.unsupported_options),
            "raw_offline": self.raw_offline,
            "live_updates": self.live_updates,
        }


def operation(
    name: str,
    worker: str,
    *,
    warning: str = "",
    description: str = "",
    label: str = "",
    unsupported_options: tuple[str, ...] = (),
) -> OperationSpec:
    """Build a concise direct-offline operation declaration."""

    layout_policy = {
        "defrag": "packed",
        "growth-defrag": "growth-10",
        "recover": "recovery",
    }[name]
    return OperationSpec(
        name=name,
        worker=worker,
        layout_policy=layout_policy,
        label=label,
        description=description,
        warning=warning,
        unsupported_options=unsupported_options,
    )


@dataclass(frozen=True, slots=True)
class BackendInfo:
    id: str
    display_name: str
    aliases: tuple[str, ...]
    capabilities: int
    map_accuracy: str = "exact"
    operations: tuple[OperationSpec, ...] = ()

    def __post_init__(self) -> None:
        if not self.id or self.id.lower() != self.id:
            raise ValueError("backend id must be a non-empty lowercase identifier")
        if not self.display_name:
            raise ValueError(f"backend {self.id} has no display name")
        aliases = tuple(alias.lower() for alias in self.aliases)
        if len(set(aliases)) != len(aliases):
            raise ValueError(f"backend {self.id} declares duplicate aliases")
        operation_names = [item.name for item in self.operations]
        if len(set(operation_names)) != len(operation_names):
            raise ValueError(f"backend {self.id} declares an operation more than once")
        capabilities = Capability(self.capabilities)
        declared = Capability(0)
        for item in self.operations:
            declared |= item.capability
        mutation_mask = Capability.DEFRAG | Capability.GROWTH_DEFRAG | Capability.RECOVER
        if capabilities & mutation_mask != declared:
            raise ValueError(
                f"backend {self.id} capability bits and operation declarations disagree: "
                f"capabilities={int(capabilities & mutation_mask)}, operations={int(declared)}"
            )
        if capabilities & (Capability.DEFRAG | Capability.GROWTH_DEFRAG):
            if not capabilities & Capability.LIVE_MAP:
                raise ValueError(
                    f"backend {self.id} declares a layout writer without live-map capability"
                )

    def operation(self, name: str) -> OperationSpec | None:
        return next((item for item in self.operations if item.name == name), None)

    def manifest(self) -> dict[str, object]:
        return {
            "id": self.id,
            "display_name": self.display_name,
            "aliases": list(self.aliases),
            "capabilities": self.capabilities,
            "map_accuracy": self.map_accuracy,
            "operations": [item.manifest() for item in self.operations],
        }


class FilesystemBackend(ABC):
    """Required read-only interface for every filesystem plugin."""

    info: BackendInfo

    @abstractmethod
    def probe(self, path: str) -> bool:
        """Return true only when *path* has this filesystem's on-disk signature."""

    @abstractmethod
    def map(self, path: str, cells: int) -> dict:
        """Return the standard allocation-map schema without modifying *path*."""
