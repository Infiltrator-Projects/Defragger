#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a raw FAT16 image that forces bounded terminal-workspace evacuation."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


output = Path(sys.argv[1])
bps = 512
spc = 1
reserved = 1
fat_count = 2
root_entries = 512
total_sectors = 64000
sectors_per_fat = 250
root_sectors = (root_entries * 32 + bps - 1) // bps
data_sector = reserved + fat_count * sectors_per_fat + root_sectors
cluster_count = (total_sectors - data_sector) // spc
max_cluster = cluster_count + 1
cluster_size = bps * spc

image = bytearray(total_sectors * bps)
boot = memoryview(image)[:bps]
boot[0:3] = b"\xeb\x3c\x90"
boot[3:11] = b"MSDOS5.0"
struct.pack_into("<H", boot, 11, bps)
boot[13] = spc
struct.pack_into("<H", boot, 14, reserved)
boot[16] = fat_count
struct.pack_into("<H", boot, 17, root_entries)
struct.pack_into("<H", boot, 19, total_sectors)
boot[21] = 0xF8
struct.pack_into("<H", boot, 22, sectors_per_fat)
boot[36] = 0x80
boot[38] = 0x29
struct.pack_into("<I", boot, 39, 0xA16B0DED)
boot[43:54] = b"WORKSPACE  "
boot[54:62] = b"FAT16   "
boot[510:512] = b"\x55\xaa"

fat = bytearray(sectors_per_fat * bps)


def set_fat(cluster: int, value: int) -> None:
    struct.pack_into("<H", fat, cluster * 2, value)


set_fat(0, 0xFFF8)
set_fat(1, 0xFFFF)
tiny_count = 192
tiny_clusters = [2 + index * 2 for index in range(tiny_count)]
for cluster in tiny_clusters:
    set_fat(cluster, 0xFFFF)

terminal = list(range(max_cluster - 63, max_cluster + 1))
large_chain = terminal[::2] + terminal[1::2]
for index, cluster in enumerate(large_chain):
    set_fat(cluster, large_chain[index + 1] if index + 1 < len(large_chain) else 0xFFFF)

for copy in range(fat_count):
    offset = (reserved + copy * sectors_per_fat) * bps
    image[offset : offset + len(fat)] = fat

root_offset = (reserved + fat_count * sectors_per_fat) * bps


def directory_entry(name: bytes, first: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = 0x20
    struct.pack_into("<H", entry, 26, first)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


for index, cluster in enumerate(tiny_clusters):
    name = f"T{index:07d}BIN".encode("ascii")
    start = root_offset + index * 32
    image[start : start + 32] = directory_entry(name, cluster, cluster_size)
    data_offset = (data_sector + cluster - 2) * bps
    image[data_offset : data_offset + cluster_size] = bytes([index % 251 + 1]) * cluster_size

large_entry = root_offset + tiny_count * 32
image[large_entry : large_entry + 32] = directory_entry(
    b"LARGE   BIN", large_chain[0], len(large_chain) * cluster_size
)
for index, cluster in enumerate(large_chain):
    data_offset = (data_sector + cluster - 2) * bps
    image[data_offset : data_offset + cluster_size] = bytes([index % 251 + 1]) * cluster_size

output.write_bytes(image)
print(output)
