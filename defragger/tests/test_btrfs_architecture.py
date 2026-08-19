#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prevent Btrfs analysis from drifting back into a Python implementation."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
PACKAGE = GUI / "filesystems" / "btrfs"
PLUGIN = PACKAGE / "plugin.py"
CMAKE = ROOT / "cmake" / "btrfs.cmake"
PATHS = GUI / "core" / "paths.py"


def main() -> None:
    native = PACKAGE / "native"
    assert native.is_dir()
    required = {"btrfs_native.h", "btrfs_native.c", "btrfs_worker.c"}
    assert required <= {path.name for path in native.iterdir() if path.is_file()}

    plugin = PLUGIN.read_text(encoding="utf-8")
    assert len(plugin.splitlines()) < 180
    assert 'resolve_program("btrfs-native"' in plugin
    for forbidden in (
        "Reader", "u16le", "u32le", "u64le", "bisect", "_TreeReader",
        "_Mapper", "_CHUNK_ITEM", "_EXTENT_ITEM", "_FILE_EXTENT_REG",
        "aggregate_ranges", "complement_ranges", "overlay_ranges",
    ):
        assert forbidden not in plugin, f"Btrfs Python parser primitive returned: {forbidden}"

    cmake = CMAKE.read_text(encoding="utf-8")
    assert "add_library(linux-defragger-btrfs-native" in cmake
    assert "add_executable(linux-defragger-btrfs-worker" in cmake
    assert (
        "install(TARGETS linux-defragger-btrfs-worker\n"
        "        RUNTIME DESTINATION lib/linux-defragger/filesystems/btrfs)"
    ) in cmake
    assert "linux-defragger-btrfs-native-python" in cmake

    paths = PATHS.read_text(encoding="utf-8")
    assert '"btrfs-native": ProgramPath(' in paths
    assert "linux-defragger-btrfs-worker" in paths

    top = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'include("${CMAKE_CURRENT_LIST_DIR}/cmake/btrfs.cmake")' in top

    print("Btrfs native C architecture guard passed")


if __name__ == "__main__":
    main()
