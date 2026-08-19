#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny single-device Btrfs metadata fixture without filesystem tools."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

SECTOR = 4096
NODE = 4096
IMAGE_SIZE = 10 * 1024 * 1024
FILESYSTEM_SIZE = 8 * 1024 * 1024
SUPER = 64 * 1024
CHUNK_LOGICAL = 1 * 1024 * 1024
CHUNK_LENGTH = 4 * 1024 * 1024
CHUNK_TREE = CHUNK_LOGICAL
ROOT_TREE = CHUNK_LOGICAL + NODE
EXTENT_TREE = CHUNK_LOGICAL + 2 * NODE
FS_TREE = CHUNK_LOGICAL + 3 * NODE
DATA1 = 2 * 1024 * 1024
DATA2 = DATA1 + 2 * SECTOR


def le16(value: int) -> bytes:
    return struct.pack("<H", value)


def le32(value: int) -> bytes:
    return struct.pack("<I", value)


def le64(value: int) -> bytes:
    return struct.pack("<Q", value)


def key(objectid: int, item_type: int, offset: int) -> bytes:
    return le64(objectid) + bytes([item_type]) + le64(offset)


def chunk_data(*, striped: bool = False) -> bytes:
    data = bytearray(48 + 32)
    data[0:8] = le64(CHUNK_LENGTH)
    data[16:24] = le64(64 * 1024)
    data[24:32] = le64(8 if striped else 1)
    data[44:46] = le16(1)
    data[48:56] = le64(1)
    data[56:64] = le64(CHUNK_LOGICAL)
    return bytes(data)


def root_item(bytenr: int) -> bytes:
    data = bytearray(239)
    data[176:184] = le64(bytenr)
    data[216:220] = le32(1)
    data[238] = 0
    return bytes(data)


def inode_item(mode: int) -> bytes:
    data = bytearray(56)
    data[52:56] = le32(mode)
    return bytes(data)


def file_extent(disk_bytenr: int) -> bytes:
    data = bytearray(53)
    data[20] = 1
    data[21:29] = le64(disk_bytenr)
    data[29:37] = le64(SECTOR)
    data[37:45] = le64(0)
    data[45:53] = le64(SECTOR)
    return bytes(data)


def leaf(bytenr: int, records: list[tuple[bytes, bytes]]) -> bytes:
    raw = bytearray(NODE)
    raw[48:56] = le64(bytenr)
    raw[96:100] = le32(len(records))
    raw[100] = 0
    table_end = 101 + 25 * len(records)
    cursor = table_end
    for index, (item_key, data) in enumerate(records):
        pos = 101 + 25 * index
        raw[pos:pos + 17] = item_key
        raw[pos + 17:pos + 21] = le32(cursor - 101)
        raw[pos + 21:pos + 25] = le32(len(data))
        raw[cursor:cursor + len(data)] = data
        cursor += len(data)
    if cursor > NODE:
        raise ValueError("fixture leaf overflow")
    return bytes(raw)


def build(path: Path, *, malformed: bool = False, multi_device: bool = False,
          striped: bool = False) -> None:
    image = bytearray(IMAGE_SIZE)
    chunk = chunk_data(striped=striped)
    system = key(256, 228, CHUNK_LOGICAL) + chunk

    superblock = bytearray(4096)
    superblock[0x40:0x48] = b"_BHRfS_M"
    superblock[80:88] = le64(ROOT_TREE)
    superblock[88:96] = le64(CHUNK_TREE)
    superblock[112:120] = le64(FILESYSTEM_SIZE)
    superblock[120:128] = le64(7 * SECTOR)
    superblock[136:144] = le64(2 if multi_device else 1)
    superblock[144:148] = le32(SECTOR)
    superblock[148:152] = le32(NODE)
    superblock[160:164] = le32(len(system))
    superblock[198] = 0
    superblock[199] = 0
    superblock[201:209] = le64(1)
    superblock[811:811 + len(system)] = system
    image[SUPER:SUPER + len(superblock)] = superblock

    image[CHUNK_TREE:CHUNK_TREE + NODE] = leaf(
        CHUNK_TREE,
        [(key(256, 228, CHUNK_LOGICAL), chunk)],
    )
    image[ROOT_TREE:ROOT_TREE + NODE] = leaf(
        ROOT_TREE,
        [
            (key(2, 132, 1), root_item(EXTENT_TREE)),
            (key(5, 132, 1), root_item(FS_TREE)),
        ],
    )
    image[EXTENT_TREE:EXTENT_TREE + NODE] = leaf(
        EXTENT_TREE,
        [
            (key(CHUNK_TREE, 169, 0), b""),
            (key(ROOT_TREE, 169, 0), b""),
            (key(EXTENT_TREE, 169, 0), b""),
            (key(FS_TREE, 169, 0), b""),
            (key(DATA1, 168, SECTOR), b""),
            (key(DATA2, 168, SECTOR), b""),
        ],
    )
    image[FS_TREE:FS_TREE + NODE] = leaf(
        FS_TREE,
        [
            (key(256, 1, 0), inode_item(0o100644)),
            (key(256, 108, 0), file_extent(DATA1)),
            (key(256, 108, SECTOR), file_extent(DATA2)),
            (key(257, 1, 0), inode_item(0o040755)),
        ],
    )

    image[DATA1:DATA1 + SECTOR] = b"A" * SECTOR
    image[DATA2:DATA2 + SECTOR] = b"B" * SECTOR

    if malformed:
        first_item = ROOT_TREE + 101
        image[first_item + 17:first_item + 21] = le32(0)

    path.write_bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--malformed", action="store_true")
    parser.add_argument("--multi-device", action="store_true")
    parser.add_argument("--striped", action="store_true")
    args = parser.parse_args()
    build(args.path, malformed=args.malformed, multi_device=args.multi_device,
          striped=args.striped)


if __name__ == "__main__":
    main()
