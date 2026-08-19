#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify canonical layout and every payload in the FAT16 workspace fixture."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


image = Path(sys.argv[1]).read_bytes()
bps = struct.unpack_from("<H", image, 11)[0]
spc = image[13]
reserved = struct.unpack_from("<H", image, 14)[0]
fat_count = image[16]
root_entries = struct.unpack_from("<H", image, 17)[0]
sectors_per_fat = struct.unpack_from("<H", image, 22)[0]
root_sectors = (root_entries * 32 + bps - 1) // bps
root_offset = (reserved + fat_count * sectors_per_fat) * bps
data_sector = reserved + fat_count * sectors_per_fat + root_sectors
cluster_size = bps * spc
fat_offset = reserved * bps


def fat_value(cluster: int) -> int:
    return struct.unpack_from("<H", image, fat_offset + cluster * 2)[0]


def cluster_bytes(cluster: int) -> bytes:
    offset = (data_sector + (cluster - 2) * spc) * bps
    return image[offset : offset + cluster_size]


entries: list[tuple[bytes, int, int]] = []
for index in range(root_entries):
    offset = root_offset + index * 32
    entry = image[offset : offset + 32]
    if entry[0] == 0:
        break
    if entry[0] == 0xE5 or entry[11] == 0x0F:
        continue
    entries.append(
        (
            entry[0:11],
            struct.unpack_from("<H", entry, 26)[0],
            struct.unpack_from("<I", entry, 28)[0],
        )
    )

assert len(entries) == 193
highest = 1
for name, first, size in entries:
    chain: list[int] = []
    cluster = first
    while True:
        chain.append(cluster)
        value = fat_value(cluster)
        if value >= 0xFFF8:
            break
        cluster = value
    assert chain == list(range(chain[0], chain[0] + len(chain))), (name, chain)
    payload = b"".join(cluster_bytes(item) for item in chain)[:size]
    if name == b"LARGE   BIN":
        expected = b"".join(bytes([index % 251 + 1]) * cluster_size for index in range(64))
    else:
        index = int(name[1:8])
        expected = bytes([index % 251 + 1]) * cluster_size
    assert payload == expected, name
    highest = max(highest, chain[-1])

for cluster in range(2, highest + 1):
    assert fat_value(cluster) != 0, ("internal free cluster", cluster)

print("verified bounded FAT16 workspace preparation, canonical layout and 193 payloads")
