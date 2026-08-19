#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stable explicit filesystem-plugin ABI.

The compatibility module keeps plugin imports short without wildcard imports or
hidden ownership. Contracts, raw reads and range aggregation remain separate
modules and are re-exported here deliberately.
"""

from .contracts import (
    BackendError,
    BackendInfo,
    Capability,
    FilesystemBackend,
    OperationSpec,
    CAP_ANALYSE,
    CAP_DEFRAG,
    CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP,
    CAP_MAP,
    CAP_RECOVER,
    operation,
)
from .io import Reader, u16be, u16le, u32be, u32le, u64be, u64le
from .ranges import (
    aggregate_bitmap,
    aggregate_ranges,
    aggregate_states,
    complement_ranges,
    count_set_bits,
    merge_ranges,
    overlay_ranges,
)

__all__ = [
    "BackendError",
    "BackendInfo",
    "Capability",
    "FilesystemBackend",
    "OperationSpec",
    "CAP_ANALYSE",
    "CAP_DEFRAG",
    "CAP_GROWTH_DEFRAG",
    "CAP_LIVE_MAP",
    "CAP_MAP",
    "CAP_RECOVER",
    "Reader",
    "aggregate_bitmap",
    "aggregate_ranges",
    "aggregate_states",
    "complement_ranges",
    "count_set_bits",
    "merge_ranges",
    "operation",
    "overlay_ranges",
    "u16be",
    "u16le",
    "u32be",
    "u32le",
    "u64be",
    "u64le",
]
