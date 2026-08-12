#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce the current C-first single-hierarchy, C-first filesystem architecture."""

from __future__ import annotations

import importlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

import operation_engine
from backends.contracts import Capability, FilesystemBackend
from backends.registry import Registry, discover_plugin_names
from core.operations import Operation
from core.paths import PROGRAMS


NATIVE_WRITERS = {
    "fat12": "fat-native",
    "fat16": "fat-native",
    "fat32": "fat-native",
    "exfat": "exfat-native",
    "ntfs": "ntfs-native",
    "ext4": "ext-native",
    "xfs": "xfs-native",
    "affs": "affs-native",
    "hfsplus": "hfsplus-native",
}


def test_plugin_discovery_and_native_worker_contracts() -> None:
    names = discover_plugin_names()
    registry = Registry()
    assert len(names) == 16
    assert len(registry.backends) == 16
    assert len(registry.ids) == len(registry.backends)

    operation_names = {item.value for item in Operation}
    writer_ids: set[str] = set()
    for name in names:
        package = importlib.import_module(f"filesystems.{name}")
        backend = package.BACKEND
        assert isinstance(backend, FilesystemBackend)
        assert backend is registry.ids[backend.info.id]
        capabilities = Capability(backend.info.capabilities)
        assert capabilities & Capability.ANALYSE
        assert capabilities & Capability.MAP
        for specification in backend.info.operations:
            writer_ids.add(backend.info.id)
            assert specification.name in operation_names
            assert specification.worker in PROGRAMS
            assert specification.raw_offline
            if specification.name in {"defrag", "growth-defrag"}:
                assert specification.live_updates
                assert capabilities & Capability.LIVE_MAP
                assert specification.worker == NATIVE_WRITERS[backend.info.id]

    assert writer_ids == set(NATIVE_WRITERS)


def test_dispatch_is_filesystem_neutral() -> None:
    registry = Registry()
    original_resolver = operation_engine.resolve_program
    try:
        operation_engine.resolve_program = lambda worker, anchor=None: f"/worker/{worker}"
        for filesystem, worker in (("fat32", "fat-native"), ("exfat", "exfat-native"),
                                   ("ntfs", "ntfs-native"), ("ext4", "ext-native"),
                                   ("xfs", "xfs-native"), ("affs", "affs-native"),
                                   ("hfsplus", "hfsplus-native")):
            command = operation_engine.build_worker_command(
                registry, filesystem, "defrag", "/dev/test", []
            )
            assert command == [f"/worker/{worker}", "defrag", "/dev/test"]
    finally:
        operation_engine.resolve_program = original_resolver


def test_single_filesystem_hierarchy_and_c_first_writers() -> None:
    assert not (ROOT / "src" / "filesystems").exists()
    assert not (ROOT / "src" / "engine").exists()
    assert not (ROOT / "src" / "core" / "ld_plugin.h").exists()
    assert not (ROOT / "src" / "linux_defragger_engine.c").exists()
    assert not (GUI / "ext_engine.py").exists()
    assert not (GUI / "ntfs_engine.py").exists()
    assert not (GUI / "exfat_engine.py").exists()
    assert not (GUI / "xfs_engine.py").exists()

    required_native = {
        "ext4": {"ext_native.h", "ext_common.c", "ext_catalog.c", "ext_plan.c", "ext_worker.c"},
        "ntfs": {"ntfs_native.h", "ntfs_common.c", "ntfs_catalog.c", "ntfs_plan.c", "ntfs_worker.c"},
        "exfat": {"exfat_native.h", "exfat_common.c", "exfat_plan.c", "exfat_worker.c"},
        "xfs": {"xfs_native.h", "xfs_common.c", "xfs_catalog.c", "xfs_plan.c", "xfs_metadata.c", "xfs_worker.c"},
        "affs": {"affs_native.h", "affs_native.c", "affs_worker.c"},
        "hfsplus": {"hfsplus_native.h", "hfsplus_native.c", "hfsplus_worker.c"},
    }
    for filesystem, native_files in required_native.items():
        package = GUI / "filesystems" / filesystem
        assert sorted(path.name for path in package.glob("*.py")) == ["__init__.py", "plugin.py"]
        native = package / "native"
        assert native.is_dir()
        assert native_files <= {path.name for path in native.iterdir() if path.is_file()}
        assert len((package / "plugin.py").read_text().splitlines()) < 260

    # A mutating filesystem may not grow a second Python implementation beside
    # its native worker. These names represent implementation logic, not GUI glue.
    forbidden_python = {
        "writer.py", "planner.py", "relocator.py", "transaction.py", "metadata.py",
        "volume.py", "catalog.py", "codec.py", "bitmap.py", "record.py", "model.py",
        "runtime.py", "staging.py", "tools.py", "libext.py", "geometry.py", "format.py",
        "placement.py",
    }
    for filesystem in ("ext4", "ntfs", "exfat", "xfs", "affs", "hfsplus"):
        package = GUI / "filesystems" / filesystem
        assert not ({path.name for path in package.glob("*.py")} & forbidden_python)

    # NTFS mutation verification must prove the exact durable plan rather than
    # delegating Growth Defrag correctness to the analyser's coarse summary.
    ntfs_plan = (GUI / "filesystems" / "ntfs" / "native" / "ntfs_plan.c").read_text()
    assert "fixed_primary" in ntfs_plan
    assert "growth&&!catalogue.growth_10_satisfied" not in ntfs_plan.replace(" ", "")
    assert "catalogue.growth_10_satisfied" not in ntfs_plan

    fat_native = GUI / "filesystems" / "fat" / "native"
    assert (fat_native / "writer.c").is_file()
    assert (GUI / "filesystems" / "hfs" / "native" / "analyser.c").is_file()


def test_build_and_path_registry_install_native_workers() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text()
    paths = (GUI / "core" / "paths.py").read_text()
    for worker, filesystem in (
        ("linux-defragger-ext-worker", "ext4"),
        ("linux-defragger-ntfs-worker", "ntfs"),
        ("linux-defragger-exfat-worker", "exfat"),
        ("linux-defragger-xfs-worker", "xfs"),
        ("linux-defragger-fat-worker", "fat"),
        ("linux-defragger-affs-worker", "affs"),
        ("linux-defragger-hfsplus-worker", "hfsplus"),
    ):
        assert worker in cmake
        install_pattern = re.compile(
            rf"install\(TARGETS\s+{re.escape(worker)}\s+"
            rf"RUNTIME DESTINATION lib/linux-defragger/filesystems/{filesystem}\)"
        )
        assert install_pattern.search(cmake), f"{worker} has no package install rule"
        assert worker in paths
    for obsolete in ("ext-raw", "ntfs-raw", "exfat-raw"):
        assert obsolete not in paths


def test_core_remains_filesystem_neutral() -> None:
    core = ROOT / "src" / "core"
    expected = {"ld_device.c", "ld_device.h", "ld_io.c", "ld_io.h", "ld_runtime.c",
                "ld_runtime.h", "ld_stage.c", "ld_stage.h", "ld_stop.c", "ld_stop.h"}
    assert expected <= {path.name for path in core.iterdir() if path.is_file()}
    combined = "\n".join(
        path.read_text(encoding="utf-8", errors="replace").lower()
        for path in core.glob("*.[ch]")
    )
    for filesystem in ("xfs", "ntfs", "exfat", "ext4", "btrfs", "hfsplus"):
        assert f"{filesystem}_" not in combined


def test_version_and_registry_are_dynamic() -> None:
    assert re.fullmatch(r"\d+\.\d+\.\d+-\d+", (ROOT / "VERSION").read_text().strip())
    registry_source = (GUI / "backends" / "registry.py").read_text()
    assert "PLUGIN_MODULES" not in registry_source
    assert "pkgutil.iter_modules" in registry_source
    launcher_lines = (GUI / "linux_defragger_gui.py").read_text().splitlines()
    assert len(launcher_lines) < 20


def main() -> None:
    test_plugin_discovery_and_native_worker_contracts()
    test_dispatch_is_filesystem_neutral()
    test_single_filesystem_hierarchy_and_c_first_writers()
    test_build_and_path_registry_install_native_workers()
    test_core_remains_filesystem_neutral()
    test_version_and_registry_are_dynamic()
    print("current C-first single-plugin architecture tests passed")


if __name__ == "__main__":
    main()
