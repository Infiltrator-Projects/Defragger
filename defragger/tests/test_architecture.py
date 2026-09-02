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
    "sfs": "sfs-native",
    "hfsplus": "hfsplus-native",
}


def _cmake_source() -> str:
    paths = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
    return "\n".join(path.read_text() for path in paths if path.is_file())


def test_top_level_cmake_owns_c_only_project_declaration() -> None:
    root_cmake = (ROOT / "CMakeLists.txt").read_text()
    project_fragment = (ROOT / "cmake" / "project.cmake").read_text()
    assert root_cmake.count("cmake_minimum_required(VERSION 3.20)") == 1
    assert root_cmake.count("project(linux_defragger VERSION 1.8.0 LANGUAGES C)") == 1
    assert root_cmake.index("cmake_minimum_required(VERSION 3.20)") < root_cmake.index("project(linux_defragger VERSION 1.8.0 LANGUAGES C)")
    assert "cmake_minimum_required(" not in project_fragment
    assert "project(" not in project_fragment


def test_plugin_discovery_and_native_worker_contracts() -> None:
    names = discover_plugin_names()
    registry = Registry()
    assert len(names) == 17
    assert len(registry.backends) == 17
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
                                   ("sfs", "sfs-native"), ("hfsplus", "hfsplus-native")):
            command = operation_engine.build_worker_command(
                registry, filesystem, "defrag", "/dev/test", []
            )
            assert command == [f"/worker/{worker}", "defrag", "/dev/test"]
    finally:
        operation_engine.resolve_program = original_resolver


def test_single_filesystem_hierarchy_and_c_first_writers() -> None:
    assert not (ROOT / "src" / "filesystems").exists()
    assert not (ROOT / "vendor").exists(), "bundled third-party source tree reintroduced"
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
        "swap": {"swap_native.h", "swap_native.c", "swap_worker.c"},
        "ufs": {"ufs_native.h", "ufs_native.c", "ufs_worker.c"},
        "zfs": {"zfs_native.h", "zfs_native.c", "zfs_worker.c"},
        "sfs": {"sfs_native.h", "sfs_native.c", "sfs_worker.c"},
        "hfs": {"analyser.c"},
    }
    for filesystem, native_files in required_native.items():
        package = GUI / "filesystems" / filesystem
        assert sorted(path.name for path in package.glob("*.py")) == ["__init__.py", "plugin.py"]
        native = package / "native"
        assert native.is_dir()
        assert native_files <= {path.name for path in native.iterdir() if path.is_file()}
        assert len((package / "plugin.py").read_text().splitlines()) < 260

    forbidden_python = {
        "writer.py", "planner.py", "relocator.py", "transaction.py", "metadata.py",
        "volume.py", "catalog.py", "codec.py", "bitmap.py", "record.py", "model.py",
        "runtime.py", "staging.py", "tools.py", "libext.py", "geometry.py", "format.py",
        "placement.py",
    }
    for filesystem in ("ext4", "ntfs", "exfat", "xfs", "affs", "sfs", "hfsplus"):
        package = GUI / "filesystems" / filesystem
        assert not ({path.name for path in package.glob("*.py")} & forbidden_python)

    ufs_plugin = (GUI / "filesystems" / "ufs" / "plugin.py").read_text()
    for forbidden in ("Reader", "_CANDIDATES", "_MAGICS", "aggregate_ranges", "data.find"):
        assert forbidden not in ufs_plugin
    assert 'resolve_program("ufs-native"' in ufs_plugin

    zfs_plugin = (GUI / "filesystems" / "zfs" / "plugin.py").read_text()
    for forbidden in ("Reader", "_UBER_MAGIC_LE", "_UBER_MAGIC_BE", "_WINDOW_SIZE", "aggregate_ranges", "data.find"):
        assert forbidden not in zfs_plugin
    assert 'resolve_program("zfs-native"' in zfs_plugin

    hfs_plugin = (GUI / "filesystems" / "hfs" / "plugin.py").read_text()
    for forbidden in ("Reader", "aggregate_bitmap", "u16be", "u32be", "bitmap_sector", "candidates ="):
        assert forbidden not in hfs_plugin
    assert 'resolve_program("hfs-native"' in hfs_plugin

    ntfs_plan = (GUI / "filesystems" / "ntfs" / "native" / "ntfs_plan.c").read_text()
    assert "fixed_primary" in ntfs_plan
    assert "growth&&!catalogue.growth_10_satisfied" not in ntfs_plan.replace(" ", "")
    assert "catalogue.growth_10_satisfied" not in ntfs_plan

    fat_native = GUI / "filesystems" / "fat" / "native"
    assert (fat_native / "writer.c").is_file()
    hfs_analyser = GUI / "filesystems" / "hfs" / "native" / "analyser.c"
    assert hfs_analyser.is_file()
    hfs_source = hfs_analyser.read_text()
    assert "libhfs" not in hfs_source
    assert "vendor/" not in _cmake_source()


def test_build_and_path_registry_install_native_workers() -> None:
    cmake = _cmake_source()
    paths = (GUI / "core" / "paths.py").read_text()
    for worker, filesystem in (
        ("linux-defragger-ext-worker", "ext4"),
        ("linux-defragger-ntfs-worker", "ntfs"),
        ("linux-defragger-exfat-worker", "exfat"),
        ("linux-defragger-xfs-worker", "xfs"),
        ("linux-defragger-fat-worker", "fat"),
        ("linux-defragger-affs-worker", "affs"),
        ("linux-defragger-hfsplus-worker", "hfsplus"),
        ("linux-defragger-swap-worker", "swap"),
        ("linux-defragger-ufs-worker", "ufs"),
        ("linux-defragger-zfs-worker", "zfs"),
        ("linux-defragger-sfs-worker", "sfs"),
    ):
        assert worker in cmake
        install_pattern = re.compile(
            rf"install\(TARGETS\s+{re.escape(worker)}\s+"
            rf"RUNTIME DESTINATION lib/linux-defragger/filesystems/{filesystem}\)"
        )
        assert install_pattern.search(cmake), f"{worker} has no package install rule"
        assert worker in paths
    assert "linux-defragger-hfs-analyser" in cmake
    assert re.search(
        r"install\(TARGETS\s+linux-defragger-hfs-analyser\s+"
        r"RUNTIME DESTINATION lib/linux-defragger/filesystems/hfs\)",
        cmake,
    )
    assert "hfs_analyser" in paths
    for obsolete in ("ext-raw", "ntfs-raw", "exfat-raw"):
        assert obsolete not in paths


def test_infiltratr_common_integration() -> None:
    common = ROOT / "shared" / "infiltratr-common"
    assert (common / "VERSION").read_text().strip() == "1.15.4"
    gitmodules = (ROOT / ".gitmodules").read_text()
    assert "shared/infiltratr-common" in gitmodules
    assert "Infiltrator-Libraries.git" in gitmodules
    cmake = _cmake_source()
    assert "046406bea2aefa539c74e1038b6c20825eca8af7" in cmake
    assert "add_subdirectory(" in cmake
    assert "InfiltratrCommon::Common" in cmake
    assert "set(INFILTRATR_COMMON_BUILD_TESTS OFF)" in cmake
    assert "set(INFILTRATR_COMMON_BUILD_SHARED OFF)" in cmake
    assert '${INFILTRATR_COMMON_DIR}/src/core.c' not in cmake
    assert '${INFILTRATR_COMMON_DIR}/src/posix.c' not in cmake
    local_installer = (ROOT / "packaging" / "build-local-run.sh").read_text()
    assert 'COMMON_VERSION="1.15.4"' in local_installer
    assert 'COMMON_COMMIT="046406bea2aefa539c74e1038b6c20825eca8af7"' in local_installer
    device = (ROOT / "src" / "core" / "ld_device.c").read_text()
    assert "infiltratr_realpath_copy" in device
    assert "infiltratr_read_u64_file" in device
    assert "infiltratr_string_starts_with" in device
    fat = (GUI / "filesystems" / "fat" / "native" / "writer.c").read_text()
    assert "infiltratr_parse_u64_range" in fat
    fat_journal = (GUI / "filesystems" / "fat" / "native" / "fat_journal.c").read_text()
    assert "infiltratr_parse_u64" in fat_journal
    assert "infiltratr_parse_u64_range" in fat_journal
    assert "infiltratr_trim_line_end" in fat_journal
    production_c = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in [*Path(ROOT / "src").rglob("*.c"),
                     *Path(GUI / "filesystems").rglob("*.c")]
    )
    assert "infiltratr_u64_add_checked" in production_c
    assert "infiltratr_u64_add_saturating" in production_c
    assert "infiltratr_array_reserve" in production_c
    assert "infiltratr_atomic_file_write" in production_c
    assert "infiltratr_unlink_durable" in production_c
    assert "ld_u64_add" not in production_c
    io = (ROOT / "src" / "core" / "ld_io.c").read_text()
    assert "infiltratr_pread_full" in io
    assert "infiltratr_pwrite_full" in io
    assert re.search(r"(?<![A-Za-z0-9_])pread\s*\(", production_c) is None
    assert re.search(r"(?<![A-Za-z0-9_])pwrite\s*\(", production_c) is None
    endian_consumers = (
        ROOT / "src" / "core" / "ld_runtime.h",
        GUI / "filesystems" / "affs" / "native" / "affs_native.c",
        GUI / "filesystems" / "btrfs" / "native" / "btrfs_native.c",
        GUI / "filesystems" / "exfat" / "native" / "exfat_common.c",
        GUI / "filesystems" / "hfs" / "native" / "analyser.c",
        GUI / "filesystems" / "hfsplus" / "native" / "hfsplus_native.c",
        GUI / "filesystems" / "sfs" / "native" / "sfs_native.c",
        GUI / "filesystems" / "swap" / "native" / "swap_native.c",
        GUI / "filesystems" / "xfs" / "native" / "xfs_common.c",
    )
    for path in endian_consumers:
        assert "infiltratr_load_" in path.read_text(), (
            f"{path.relative_to(ROOT)} bypasses Common endian access"
        )
    for filesystem, worker in (("ext4", "ext_worker.c"), ("ntfs", "ntfs_worker.c"),
                               ("exfat", "exfat_worker.c"), ("xfs", "xfs_worker.c")):
        source = (GUI / "filesystems" / filesystem / "native" / worker).read_text()
        assert "infiltratr_parse_u64" in source
        assert "infiltratr_trim_line_end" in source
    for filesystem, worker in (("affs", "affs_worker.c"), ("sfs", "sfs_worker.c"),
                               ("hfsplus", "hfsplus_worker.c")):
        source = (GUI / "filesystems" / filesystem / "native" / worker).read_text()
        assert "infiltratr_parse_u64_range" in source
        assert "infiltratr_trim_line_end" in source


def test_core_remains_filesystem_neutral() -> None:
    core = ROOT / "src" / "core"
    expected = {"ld_device.c", "ld_device.h", "ld_io.c", "ld_io.h", "ld_runtime.c",
                "ld_runtime.h", "ld_path.c", "ld_path.h", "ld_stop.c", "ld_stop.h"}
    assert expected <= {path.name for path in core.iterdir() if path.is_file()}
    combined = "\n".join(
        path.read_text(encoding="utf-8", errors="replace").lower()
        for path in core.glob("*.[ch]")
    )
    for filesystem in ("xfs", "ntfs", "exfat", "ext4", "btrfs", "hfsplus"):
        assert f"{filesystem}_" not in combined


def test_production_write_safety_is_enforced_at_every_boundary() -> None:
    runtime_header = (ROOT / "src" / "core" / "ld_runtime.h").read_text()
    runtime_source = (ROOT / "src" / "core" / "ld_runtime.c").read_text()
    engine = (GUI / "engine" / "cli.py").read_text()
    planner = (GUI / "ui" / "operation_planner.py").read_text()
    combined_policy = "\n".join((runtime_header, runtime_source, engine, planner))
    assert "UNAUDITED_RAW_WRITES" not in combined_policy
    assert "require_unmounted(args.device)" in engine
    assert "if volume.mounted:" in planner

    native = GUI / "filesystems"
    workers = {
        "fat": native / "fat" / "native" / "writer.c",
        "ext": native / "ext4" / "native" / "ext_worker.c",
        "ntfs": native / "ntfs" / "native" / "ntfs_worker.c",
        "exfat": native / "exfat" / "native" / "exfat_worker.c",
        "xfs": native / "xfs" / "native" / "xfs_worker.c",
        "affs": native / "affs" / "native" / "affs_worker.c",
        "sfs": native / "sfs" / "native" / "sfs_worker.c",
        "hfsplus": native / "hfsplus" / "native" / "hfsplus_worker.c",
    }
    sources = {name: path.read_text() for name, path in workers.items()}
    for name, source in sources.items():
        assert "--write" in source and "--confirm" in source, (
            f"{name} lost explicit mutation confirmation"
        )
        assert "ld_runtime_require_write_audit_override" not in source

    assert "ld_device_open(device_path, mutating)" in sources["fat"]
    assert sources["ext"].count("ld_path_is_mounted(device)") >= 2
    assert sources["ntfs"].count("ld_path_is_mounted(device)") >= 2
    assert "ld_path_is_mounted(device)" in sources["exfat"]
    assert "ld_device_number_is_mounted(status.st_rdev)" in sources["xfs"]
    assert "ld_path_is_mounted(device)" in sources["affs"]
    assert "ld_path_is_mounted(device)" in sources["sfs"]
    assert "ld_path_is_mounted(device)" in sources["hfsplus"]

    recovery_bindings = {
        "ext": (".ext-stage.img", ".ext-plan.sqlite"),
        "ntfs": (".ntfs-stage.img", ".ntfs-plan.sqlite"),
        "exfat": (".exfat-stage.img",),
        "xfs": (".xfs-stage.img", ".xfs-plan.sqlite"),
        "affs": (".affs-stage",),
        "sfs": (".sfs-stage",),
        "hfsplus": (".hfsplus-stage",),
    }
    for name, suffixes in recovery_bindings.items():
        assert "ld_path_is_derived_from" in sources[name]
        for suffix in suffixes:
            assert suffix in sources[name], f"{name} lost {suffix} recovery binding"

    journal_sources = {
        "fat": native / "fat" / "native" / "fat_journal.c",
        "ext": workers["ext"],
        "ntfs": workers["ntfs"],
        "exfat": workers["exfat"],
        "xfs": workers["xfs"],
        "affs": workers["affs"],
        "sfs": workers["sfs"],
        "hfsplus": workers["hfsplus"],
    }
    for name, path in journal_sources.items():
        assert "infiltratr_atomic_file_write" in path.read_text(), (
            f"{name} journal lost Common durable atomic-file handling"
        )
    assert "infiltratr_atomic_file_write" in (
        native / "exfat" / "native" / "exfat_relayout.c"
    ).read_text()

    for path in (
        native / "ext4" / "native" / "ext_catalog.c",
        native / "ntfs" / "native" / "ntfs_plan.c",
        native / "xfs" / "native" / "xfs_plan.c",
    ):
        assert "SQLITE_OPEN_NOFOLLOW" in path.read_text(), (
            f"{path.relative_to(ROOT)} lost SQLite symlink refusal"
        )

    # Persistent stages can be created by a root worker below a user-owned state
    # directory. They must never follow or truncate a pre-created symlink.
    for path in (
        native / "exfat" / "native" / "exfat_plan.c",
        native / "affs" / "native" / "affs_native.c",
        native / "sfs" / "native" / "sfs_native.c",
        native / "hfsplus" / "native" / "hfsplus_native.c",
    ):
        stage_source = path.read_text()
        assert "O_EXCL" in stage_source, (
            f"{path.relative_to(ROOT)} lost exclusive stage creation"
        )
        assert "O_NOFOLLOW" in stage_source, (
            f"{path.relative_to(ROOT)} lost stage symlink refusal"
        )
        assert "O_TRUNC" not in stage_source, (
            f"{path.relative_to(ROOT)} can truncate a pre-existing stage target"
        )

    assert "an unfinished NTFS journal exists; run Recover first" in sources["ntfs"]


def test_version_and_registry_are_dynamic() -> None:
    assert re.fullmatch(r"\d+\.\d+\.\d+-\d+", (ROOT / "VERSION").read_text().strip())
    registry_source = (GUI / "backends" / "registry.py").read_text()
    assert "PLUGIN_MODULES" not in registry_source
    assert "pkgutil.iter_modules" in registry_source
    launcher_lines = (GUI / "linux_defragger_gui.py").read_text().splitlines()
    assert len(launcher_lines) < 20


def test_test_media_companion_is_all_c() -> None:
    source_dir = ROOT / "test_media"
    assert source_dir.is_dir()
    required = {
        "test_media.h",
        "test_media_core.c",
        "test_media_worker.c",
        "test_media_main.c",
        "test_media_gui.c",
    }
    assert required.issubset({path.name for path in source_dir.iterdir() if path.is_file()})
    assert not list(source_dir.glob("*.py"))
    assert not (ROOT / "tools" / "linux-defragger-media-harness.py").exists()
    assert not (ROOT / "tools" / "linux-defragger-testdata.py").exists()
    assert not (ROOT / "tests" / "test_media_harness.py").exists()

    cmake = _cmake_source()
    assert "add_executable(linux-defragger-test-media" in cmake
    assert "install(TARGETS linux-defragger-test-media RUNTIME DESTINATION bin)" in cmake
    assert "linux-defragger-media-harness" not in cmake
    assert "linux-defragger-testdata" not in cmake

    desktop = (ROOT / "packaging" / "io.github.linuxdefragger.TestMedia.desktop").read_text()
    assert "Exec=linux-defragger-test-media" in desktop

    window = (GUI / "ui" / "window.py").read_text()
    window_view = (GUI / "ui" / "window_view.py").read_text()
    install_script = (ROOT / "install.sh").read_text()
    assert "linux-defragger-testdata" not in window
    assert "Create fragmented test data" not in window_view
    assert "linux-defragger-testdata" not in install_script
    assert 'cmake --install "$BUILD" --prefix /usr' in install_script


def main() -> None:
    test_top_level_cmake_owns_c_only_project_declaration()
    test_plugin_discovery_and_native_worker_contracts()
    test_dispatch_is_filesystem_neutral()
    test_single_filesystem_hierarchy_and_c_first_writers()
    test_build_and_path_registry_install_native_workers()
    test_infiltratr_common_integration()
    test_core_remains_filesystem_neutral()
    test_production_write_safety_is_enforced_at_every_boundary()
    test_test_media_companion_is_all_c()
    test_version_and_registry_are_dynamic()
    print("current C-first single-plugin architecture tests passed")


if __name__ == "__main__":
    main()
