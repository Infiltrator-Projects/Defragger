#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression for the first-party classic-HFS native analyser."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
ANALYSER = BUILD / "hfs_analyser"
EXPECTED_EXTENTS = [[10, 1], [20, 1], [30, 1]]


def _be16(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", buffer, offset, value)


def _be32(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", buffer, offset, value)


def _node_record(node: bytearray, index: int, start: int, end: int) -> None:
    _be16(node, 512 - 2 * (index + 1), start)
    _be16(node, 512 - 2 * (index + 2), end)


def _btree_header_node() -> bytes:
    node = bytearray(512)
    node[8] = 1  # header node
    _be16(node, 10, 1)
    start, end = 14, 64
    _node_record(node, 0, start, end)
    _be32(node, start + 10, 1)  # first leaf
    _be32(node, start + 14, 1)  # last leaf
    _be16(node, start + 18, 512)
    _be32(node, start + 22, 2)  # total nodes
    return bytes(node)


def _empty_leaf_node() -> bytes:
    node = bytearray(512)
    node[8] = 0xFF  # leaf node (-1)
    _be16(node, 10, 0)
    return bytes(node)


def _catalog_leaf_node() -> bytes:
    node = bytearray(512)
    node[8] = 0xFF
    _be16(node, 10, 1)
    start, end = 14, 124
    _node_record(node, 0, start, end)
    record = memoryview(node)[start:end]
    record[0] = 6  # catalog key length; padded key occupies 8 bytes
    _be32(node, start + 2, 2)  # parent CNID
    data = start + 8
    node[data] = 2  # file record
    _be32(node, data + 20, 16)  # file CNID
    _be32(node, data + 30, 3 * 512)  # physical data-fork length
    _be32(node, data + 40, 0)  # resource-fork length
    for index, block in enumerate((10, 20, 30)):
        _be16(node, data + 74 + index * 4, block)
        _be16(node, data + 76 + index * 4, 1)
    return bytes(node)


def _make_fragmented_hfs(path: Path) -> None:
    image = bytearray(128 * 1024)
    mdb = memoryview(image)[1024:1024 + 162]
    _be16(image, 1024, 0x4244)
    _be16(image, 1024 + 18, 200)  # allocation blocks
    _be32(image, 1024 + 20, 512)  # allocation block size
    _be16(image, 1024 + 28, 4)  # first allocation block in 512-byte sectors
    _be32(image, 1024 + 84, 1)  # file count
    _be32(image, 1024 + 88, 0)  # directory count (catalog records only here)
    _be32(image, 1024 + 130, 1024)  # extents-overflow file size
    _be16(image, 1024 + 134, 0)
    _be16(image, 1024 + 136, 2)
    _be32(image, 1024 + 146, 1024)  # catalog file size
    _be16(image, 1024 + 150, 2)
    _be16(image, 1024 + 152, 2)
    del mdb

    allocation_base = 4 * 512
    image[allocation_base:allocation_base + 512] = _btree_header_node()
    image[allocation_base + 512:allocation_base + 1024] = _empty_leaf_node()
    catalog_base = allocation_base + 2 * 512
    image[catalog_base:catalog_base + 512] = _btree_header_node()
    image[catalog_base + 512:catalog_base + 1024] = _catalog_leaf_node()
    path.write_bytes(image)


def main() -> None:
    assert ANALYSER.is_file() and os.access(ANALYSER, os.X_OK), ANALYSER
    assert not (ROOT / "vendor").exists()
    with tempfile.TemporaryDirectory(prefix="linux-defragger-hfs-test.") as tmp:
        image = Path(tmp) / "fragmented-hfs.img"
        _make_fragmented_hfs(image)
        completed = subprocess.run(
            [str(ANALYSER), "scan-json", str(image)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        result = json.loads(completed.stdout)
        assert result["files"] == 1
        assert result["directories"] == 0
        assert result["fragmented_files"] == 1
        assert result["fragmented_directories"] == 0
        assert result["fragmented_extents"] == EXPECTED_EXTENTS

        rejected = subprocess.run(
            [str(ANALYSER), "defrag", str(image)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert rejected.returncode != 0

    print("Classic HFS first-party native analyser regression passed")


if __name__ == "__main__":
    main()
