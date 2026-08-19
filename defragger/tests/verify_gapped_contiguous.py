#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the packed Defragment result for initially contiguous gapped FAT files."""

import struct
import sys
from pathlib import Path

p = Path(sys.argv[1])
file_count = 64
clusters_per_file = 8
with p.open("rb") as f:
    boot = f.read(512)
    bps = struct.unpack_from("<H", boot, 11)[0]
    spc = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    fatsz = struct.unpack_from("<I", boot, 36)[0]
    root = struct.unpack_from("<I", boot, 44)[0] & 0x0FFFFFFF
    cluster_size = bps * spc
    data_sector = reserved + fats * fatsz

    def cluster_offset(cluster: int) -> int:
        return data_sector * bps + (cluster - 2) * cluster_size

    f.seek(reserved * bps)
    fat = f.read(fatsz * bps)

    def fat_value(cluster: int) -> int:
        return struct.unpack_from("<I", fat, cluster * 4)[0] & 0x0FFFFFFF

    f.seek(cluster_offset(root))
    entries = [f.read(32) for _ in range(file_count)]
    highest = 1
    fragmented = 0
    for file_index, entry in enumerate(entries):
        first = (
            (struct.unpack_from("<H", entry, 20)[0] << 16)
            | struct.unpack_from("<H", entry, 26)[0]
        ) & 0x0FFFFFFF
        chain: list[int] = []
        seen: set[int] = set()
        cluster = first
        while 2 <= cluster < 0x0FFFFFF8:
            assert cluster not in seen, (file_index, "loop", cluster)
            seen.add(cluster)
            chain.append(cluster)
            cluster = fat_value(cluster)
        assert len(chain) == clusters_per_file, (file_index, len(chain))
        fragments = 1 + sum(b != a + 1 for a, b in zip(chain, chain[1:]))
        fragmented += fragments > 1
        for logical, physical in enumerate(chain):
            f.seek(cluster_offset(physical))
            assert f.read(2) == bytes([file_index, logical]), (
                file_index,
                logical,
                physical,
            )
        highest = max(highest, max(chain))

    for cluster in range(2, highest + 1):
        assert fat_value(cluster) != 0, ("internal free cluster", cluster)
    assert fragmented == 0, f"Defragment created fragmentation in {fragmented} files"

print("verified packed FAT Defragment layout: no internal gaps and all chains contiguous")
