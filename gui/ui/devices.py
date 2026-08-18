# SPDX-License-Identifier: GPL-3.0-or-later
"""Block-device discovery and filesystem-neutral volume model."""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable

from .formatting import human_bytes
from .backend_catalog import BackendCatalog


RunCommand = Callable[..., subprocess.CompletedProcess[str]]
_GENERIC_FAT_TYPES = frozenset({"vfat", "fat", "msdos"})
_NATURAL_DEVICE_PARTS = re.compile(r"(\d+)")


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


def natural_device_sort_key(path: str) -> tuple[tuple[int, object], ...]:
    """Sort Linux block-device paths by numeric components, not text order.

    This keeps partitions grouped in the order humans expect, for example
    ``mmcblk0p1, mmcblk0p2, ... mmcblk0p10`` and
    ``nvme0n1p1, nvme0n1p2, ... nvme0n1p12``.
    """

    parts: list[tuple[int, object]] = []
    for part in _NATURAL_DEVICE_PARTS.split(path.casefold()):
        if not part:
            continue
        if part.isdigit():
            parts.append((1, int(part)))
        else:
            parts.append((0, part))
    return tuple(parts)


def fat_variant(fstype: str, fs_version: str) -> str:
    """Return FAT12/FAT16/FAT32 when Linux metadata is precise enough."""

    raw = str(fstype or "").strip().lower()
    if raw in {"fat12", "fat16", "fat32"}:
        return raw
    if raw not in _GENERIC_FAT_TYPES:
        return ""
    version = str(fs_version or "").strip().lower().replace(" ", "")
    for variant in ("fat12", "fat16", "fat32"):
        if version in {variant, variant[3:]}:
            return variant
    return ""


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
    fs_version: str = ""
    filesystem_uuid: str = ""
    partition_uuid: str = ""
    _cache_nonce: object = field(
        default_factory=object,
        init=False,
        repr=False,
        compare=False,
    )

    @property
    def mounted(self) -> bool:
        return any(self.mountpoints)

    @property
    def normalized_fstype(self) -> str:
        variant = fat_variant(self.fstype, self.fs_version)
        return variant or self.catalog.normalize(self.fstype)

    @property
    def display_fstype(self) -> str:
        raw = self.fstype.strip().lower()
        variant = fat_variant(raw, self.fs_version)
        if variant:
            return variant
        if raw in _GENERIC_FAT_TYPES:
            return "fat"
        return self.normalized_fstype

    @property
    def capabilities(self) -> int:
        return self.catalog.capabilities_for(self.normalized_fstype)

    @property
    def cache_key(self) -> tuple[object, ...]:
        """Identify the filesystem instance, not merely its reusable path."""

        if self.image:
            try:
                stat = os.stat(self.path)
            except OSError:
                return ("image-ephemeral", self.path, self._cache_nonce)
            return (
                "image",
                self.path,
                stat.st_dev,
                stat.st_ino,
                stat.st_size,
                stat.st_mtime_ns,
                self.normalized_fstype,
            )

        filesystem_uuid = self.filesystem_uuid.strip().lower()
        partition_uuid = self.partition_uuid.strip().lower()
        if filesystem_uuid or partition_uuid:
            return (
                "device",
                self.path,
                filesystem_uuid,
                partition_uuid,
                self.normalized_fstype,
                self.size,
            )

        # A filesystem with no stable UUID must not inherit analysis from a
        # different object discovered later at the same device path.
        return ("device-ephemeral", self.path, self._cache_nonce)

    @property
    def display_name(self) -> str:
        label = self.label or self.model or self.name
        status = "mounted" if self.mounted else "unmounted"
        kind = "image" if self.image else (self.transport or "device")
        filesystem = self.display_fstype.upper()
        return (
            f"{self.path} — {label} — {filesystem} — {human_bytes(self.size)} — "
            f"{kind}, {status}"
        )


def discover_volumes(
    catalog: BackendCatalog,
    *,
    run: RunCommand = subprocess.run,
) -> list[Volume]:
    columns = (
        "NAME,PATH,TYPE,FSTYPE,FSVER,LABEL,UUID,PARTUUID,SIZE,"
        "MOUNTPOINTS,RM,RO,MODEL,TRAN"
    )
    result = run(
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
                mountpoints=[
                    str(item)
                    for item in (node.get("mountpoints") or [])
                    if item
                ],
                removable=json_bool(node.get("rm")),
                readonly=json_bool(node.get("ro")),
                model=str(node.get("model") or "").strip(),
                transport=str(node.get("tran") or ""),
                fs_version=str(node.get("fsver") or ""),
                filesystem_uuid=str(node.get("uuid") or ""),
                partition_uuid=str(node.get("partuuid") or ""),
            )
        )
    volumes.sort(
        key=lambda volume: (
            not volume.removable,
            natural_device_sort_key(volume.path),
        )
    )
    return volumes
