#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mount and block-topology safety shared by filesystem mutation workers."""

from __future__ import annotations

import os
import re
import stat
from pathlib import Path

_MOUNTINFO = Path("/proc/self/mountinfo")
_SYS_DEV_BLOCK = Path("/sys/dev/block")
_MOUNT_ESCAPE = re.compile(r"\\([0-7]{3})")
_DEVICE_ID = re.compile(r"^\d+:\d+$")


def _decode_mount_field(value: str) -> str:
    return _MOUNT_ESCAPE.sub(lambda match: chr(int(match.group(1), 8)), value)


def _mount_records() -> list[tuple[str, str]]:
    """Return ``(major:minor, source)`` pairs from the current mount namespace."""

    records: list[tuple[str, str]] = []
    try:
        lines = _MOUNTINFO.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return records
    for line in lines:
        fields = line.split()
        separator = fields.index("-") if "-" in fields else -1
        if separator < 0 or len(fields) <= separator + 2 or len(fields) <= 2:
            continue
        records.append((fields[2], _decode_mount_field(fields[separator + 2])))
    return records


def _block_nodes() -> dict[str, Path]:
    """Return every kernel block ``major:minor`` and its resolved sysfs node."""

    nodes: dict[str, Path] = {}
    try:
        entries = tuple(_SYS_DEV_BLOCK.iterdir())
    except OSError:
        return nodes
    for entry in entries:
        if not _DEVICE_ID.fullmatch(entry.name):
            continue
        try:
            nodes[entry.name] = entry.resolve()
        except OSError:
            continue
    return nodes


def _path_contains(parent: Path, child: Path) -> bool:
    try:
        child.relative_to(parent)
        return parent != child
    except ValueError:
        return False


def _direct_block_relations(item: str, nodes: dict[str, Path]) -> set[str]:
    """Return holder/slave devices that map storage to or from *item*."""

    path = nodes.get(item)
    if path is None:
        return set()
    by_path = {str(node): device_id for device_id, node in nodes.items()}
    related: set[str] = set()
    for relation_name in ("holders", "slaves"):
        relation = path / relation_name
        try:
            children = tuple(relation.iterdir())
        except OSError:
            continue
        for child in children:
            try:
                device_id = by_path.get(str(child.resolve()))
            except OSError:
                device_id = None
            if device_id is None:
                try:
                    device_id = (child / "dev").read_text(encoding="ascii").strip()
                except OSError:
                    continue
            if device_id in nodes:
                related.add(device_id)
    return related


def _ancestor_block_ids(item: str, nodes: dict[str, Path]) -> set[str]:
    """Return whole-device ancestors whose address space contains *item*."""

    path = nodes.get(item)
    if path is None:
        return set()
    return {
        device_id
        for device_id, candidate in nodes.items()
        if device_id != item and _path_contains(candidate, path)
    }


def _descendant_block_ids(item: str, nodes: dict[str, Path]) -> set[str]:
    """Return partitions/subdevices physically contained by *item*."""

    path = nodes.get(item)
    if path is None:
        return set()
    return {
        device_id
        for device_id, candidate in nodes.items()
        if device_id != item and _path_contains(path, candidate)
    }


def _related_block_ids(device_id: str) -> set[str]:
    """Return block nodes whose mounted state can overlap *device_id*.

    A partition mutation must reject the partition itself, its whole-device
    ancestor, and holder/slave mappings that genuinely overlap it.  Mounted
    sibling partitions are deliberately excluded: they occupy disjoint ranges
    of the parent disk and do not make an unmounted target partition unsafe.

    When the target itself is a whole block device, its child partitions are
    included because writing the whole device necessarily overlaps them.
    """

    nodes = _block_nodes()
    if device_id not in nodes:
        return {device_id}

    result: set[str] = set()
    pending: list[tuple[str, bool]] = [(device_id, True)]
    while pending:
        item, exact_mapping = pending.pop()
        if item in result:
            continue
        result.add(item)

        ancestors = _ancestor_block_ids(item, nodes)
        for ancestor in ancestors - result:
            # Ancestors overlap this exact partition, but their other child
            # partitions do not.  Mark them as containers only.
            pending.append((ancestor, False))

        # A whole-device target or an exact holder/slave mapping overlaps its
        # child partitions.  Do not descend through a parent that was added
        # merely because it contains the selected partition.
        if exact_mapping and not ancestors:
            for child in _descendant_block_ids(item, nodes) - result:
                pending.append((child, True))

        # Holder/slave relationships represent storage mappings rather than
        # sibling partitions, so they remain part of the overlap closure.
        for related in _direct_block_relations(item, nodes) - result:
            pending.append((related, True))

    return result

def _loop_ids_for_backing_file(real_path: str) -> set[str]:
    """Return loop devices whose kernel backing file is *real_path*."""

    matches: set[str] = set()
    for device_id, node in _block_nodes().items():
        try:
            raw = (node / "loop" / "backing_file").read_text(
                encoding="utf-8", errors="replace"
            ).strip()
        except OSError:
            continue
        if not raw:
            continue
        candidates = (raw, "/" + raw.lstrip("/"))
        if any(os.path.realpath(candidate) == real_path for candidate in candidates):
            matches.update(_related_block_ids(device_id))
    return matches


def is_mounted(path: str) -> bool:
    """Return whether *path* or any related block topology node is mounted."""

    real = os.path.realpath(path)
    try:
        target = os.stat(real)
    except OSError:
        return False

    related: set[str] = set()
    if stat.S_ISBLK(target.st_mode):
        device_id = f"{os.major(target.st_rdev)}:{os.minor(target.st_rdev)}"
        related = _related_block_ids(device_id)
    elif stat.S_ISREG(target.st_mode):
        related = _loop_ids_for_backing_file(real)

    for mounted_id, source in _mount_records():
        if mounted_id in related:
            return True
        if source and os.path.realpath(source) == real:
            return True
    return False


def require_unmounted(path: str, *, block_device: bool = False) -> None:
    """Validate a mutation target before a worker opens it for writing."""

    try:
        target = os.stat(path)
    except OSError as exc:
        raise RuntimeError(f"cannot inspect target {path}: {exc}") from exc
    if block_device and not stat.S_ISBLK(target.st_mode):
        raise RuntimeError("this operation requires a real block-device partition")
    if is_mounted(path):
        raise RuntimeError(
            f"{path} or an overlapping parent, child, holder, slave or loop mapping is mounted; "
            "unmount only devices that overlap the selected target before mutation"
        )
