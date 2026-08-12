# SPDX-License-Identifier: GPL-3.0-or-later
"""Block-device discovery and filesystem-neutral volume model."""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from typing import Any, Iterable

from .formatting import human_bytes
from .backend_catalog import BackendCatalog


def json_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    return str(value or "").strip().lower() in {"1", "true", "yes", "on"}


def flatten_lsblk(nodes: Iterable[dict[str, Any]]) -> Iterable[dict[str, Any]]:
    for node in nodes:
        yield node
        yield from flatten_lsblk(node.get("children") or [])


@dataclass(slots=True)
class Volume:
    catalog: BackendCatalog
    path: str
    name: str
    fstype: str
    label: str
    size: int
    mountpoints: list[str]
    removable: bool
    readonly: bool
    model: str
    transport: str
    image: bool = False

    @property
    def mounted(self) -> bool:
        return any(self.mountpoints)

    @property
    def normalized_fstype(self) -> str:
        return self.catalog.normalize(self.fstype)

    @property
    def capabilities(self) -> int:
        return self.catalog.capabilities_for(self.fstype)

    @property
    def display_name(self) -> str:
        label = self.label or self.model or self.name
        status = "mounted" if self.mounted else "unmounted"
        kind = "image" if self.image else (self.transport or "device")
        filesystem = self.normalized_fstype.upper()
        return (
            f"{self.path} — {label} — {filesystem} — {human_bytes(self.size)} — "
            f"{kind}, {status}"
        )


def discover_volumes(catalog: BackendCatalog) -> list[Volume]:
    columns = "NAME,PATH,TYPE,FSTYPE,LABEL,SIZE,MOUNTPOINTS,RM,RO,MODEL,TRAN"
    result = subprocess.run(
        ["lsblk", "--json", "--bytes", "--output", columns],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C"},
    )
    data = json.loads(result.stdout)
    volumes: list[Volume] = []
    for node in flatten_lsblk(data.get("blockdevices", [])):
        fstype = str(node.get("fstype") or "")
        if not catalog.supports(fstype):
            continue
        volumes.append(
            Volume(
                catalog=catalog,
                path=str(node.get("path") or ""),
                name=str(node.get("name") or ""),
                fstype=fstype,
                label=str(node.get("label") or ""),
                size=int(node.get("size") or 0),
                mountpoints=[str(item) for item in (node.get("mountpoints") or []) if item],
                removable=json_bool(node.get("rm")),
                readonly=json_bool(node.get("ro")),
                model=str(node.get("model") or "").strip(),
                transport=str(node.get("tran") or ""),
            )
        )
    volumes.sort(key=lambda volume: (not volume.removable, volume.path))
    return volumes
