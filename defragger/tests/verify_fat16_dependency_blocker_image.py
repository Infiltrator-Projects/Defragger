#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify canonical 10% Growth placement and payload integrity for blocker image."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


path = Path(sys.argv[1])
bps = 512
blocker_clusters = 5000
medium_files = 299
medium_clusters = 100
blocker_reserve = 500
medium_reserve = 10

with path.open("rb") as handle:
    boot = handle.read(512)
    bps = struct.unpack_from("<H", boot, 11)[0]
    spc = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fat_count = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
    root_sectors = (root_entries * 32 + bps - 1) // bps
    data_sector = reserved + fat_count * sectors_per_fat + root_sectors
    cluster_size = bps * spc
    root_offset = (reserved + fat_count * sectors_per_fat) * bps

    handle.seek(reserved * bps)
    fat = handle.read(sectors_per_fat * bps)

    def fat_value(cluster: int) -> int:
        return struct.unpack_from("<H", fat, cluster * 2)[0]

    def read_chain(first: int) -> list[int]:
        chain: list[int] = []
        cluster = first
        seen: set[int] = set()
        while True:
            assert cluster not in seen, ("cycle", cluster)
            seen.add(cluster)
            chain.append(cluster)
            nxt = fat_value(cluster)
            if nxt >= 0xFFF8:
                return chain
            assert 2 <= nxt < 0xFFF8, (cluster, nxt)
            cluster = nxt

    def cluster_offset(cluster: int) -> int:
        return (data_sector + (cluster - 2) * spc) * bps

    expected_target = 2
    for entry_index in range(1 + medium_files):
        handle.seek(root_offset + entry_index * 32)
        entry = handle.read(32)
        first = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]
        expected_clusters = blocker_clusters if entry_index == 0 else medium_clusters
        expected_payload = 0xA5 if entry_index == 0 else entry_index % 251 + 1
        chain = read_chain(first)
        assert len(chain) == expected_clusters, (entry_index, len(chain))
        assert chain == list(range(expected_target, expected_target + expected_clusters)), (
            entry_index,
            chain[:4],
            chain[-4:],
            expected_target,
        )
        assert size == expected_clusters * cluster_size
        for cluster in chain:
            handle.seek(cluster_offset(cluster))
            payload = handle.read(cluster_size)
            assert payload == bytes([expected_payload]) * cluster_size, (
                "payload",
                entry_index,
                cluster,
            )

        reserve = blocker_reserve if entry_index == 0 else medium_reserve
        for cluster in range(expected_target + expected_clusters,
                             expected_target + expected_clusters + reserve):
            assert fat_value(cluster) == 0, ("reserve", entry_index, cluster, fat_value(cluster))
        expected_target += expected_clusters + reserve

print("verified FAT16 adaptive dependency Growth layout and payload integrity")
