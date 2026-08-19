#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a FAT16 image whose fragmented early file blocks hundreds of Growth targets.

The live set is deliberately larger than the canonical tail workspace, so the
all-or-nothing workspace fast path cannot solve it.  A fragmented first file
owns one cluster in every later file's Growth target, reproducing the physical
FAT16 dependency pattern that previously collapsed into one-file/three-
transaction staging.
"""

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

blocker_clusters = 5000
medium_files = 299
medium_clusters = 100
blocker_reserve = (blocker_clusters * 10 + 99) // 100
medium_reserve = (medium_clusters * 10 + 99) // 100

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
struct.pack_into("<I", boot, 39, 0x16AD1223)
boot[43:54] = b"DEP-BLOCK  "
boot[54:62] = b"FAT16   "
boot[510:512] = b"\x55\xaa"

# Begin with a densely packed live set.  The Growth target of every medium file
# is shifted right by the reserve after earlier files.
blocker = list(range(2, 2 + blocker_clusters))
medium: list[list[int]] = []
cursor = 2 + blocker_clusters
for _index in range(medium_files):
    medium.append(list(range(cursor, cursor + medium_clusters)))
    cursor += medium_clusters
live_end = cursor - 1

first_medium_target = 2 + blocker_clusters + blocker_reserve
target_starts = [
    first_medium_target + index * (medium_clusters + medium_reserve)
    for index in range(medium_files)
]
target_set = set(target_starts)
layout_end = target_starts[-1] + medium_clusters + medium_reserve - 1
assert layout_end < max_cluster

# Scatter 299 clusters from near the end of BLOCKER.BIN into the first cluster
# of every medium file's eventual Growth target.  If the target is currently
# owned by a medium file, move that medium cluster into an otherwise-free slot
# below the eventual terminal workspace.  Physical-first ordering is preserved.
replacement_cursor = live_end + 1
for index, target in enumerate(target_starts, start=1):
    low = 2 + blocker_clusters - 1 - index
    blocker[low - 2] = target

    if 2 + blocker_clusters <= target <= live_end:
        owner = (target - (2 + blocker_clusters)) // medium_clusters
        position = (target - (2 + blocker_clusters)) % medium_clusters
        while replacement_cursor in target_set:
            replacement_cursor += 1
        assert replacement_cursor <= layout_end
        medium[owner][position] = replacement_cursor
        replacement_cursor += 1

all_chains = [blocker, *medium]
flat = [cluster for chain in all_chains for cluster in chain]
assert len(flat) == blocker_clusters + medium_files * medium_clusters
assert len(set(flat)) == len(flat)
assert min(flat) >= 2 and max(flat) <= max_cluster

fat = bytearray(sectors_per_fat * bps)


def set_fat(cluster: int, value: int) -> None:
    struct.pack_into("<H", fat, cluster * 2, value)


set_fat(0, 0xFFF8)
set_fat(1, 0xFFFF)
for chain in all_chains:
    for position, cluster in enumerate(chain):
        next_cluster = chain[position + 1] if position + 1 < len(chain) else 0xFFFF
        set_fat(cluster, next_cluster)

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


entries = [(b"BLOCKER BIN", blocker, 0xA5)]
for index, chain in enumerate(medium, start=1):
    entries.append((f"F{index:07d}BIN".encode("ascii"), chain, index % 251 + 1))

for entry_index, (name, chain, payload_byte) in enumerate(entries):
    start = root_offset + entry_index * 32
    image[start : start + 32] = directory_entry(
        name,
        chain[0],
        len(chain) * cluster_size,
    )
    for cluster in chain:
        data_offset = (data_sector + cluster - 2) * bps
        image[data_offset : data_offset + cluster_size] = bytes([payload_byte]) * cluster_size

output.write_bytes(image)
print(output)
