#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ensure release publication cannot bypass the permanent project quality gate."""

from __future__ import annotations

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PROJECT_ROOT.parent


def main() -> None:
    gate = (REPO_ROOT / ".github" / "workflows" / "quality-gate.yml").read_text(encoding="utf-8")
    release = (REPO_ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    harness = (PROJECT_ROOT / "tests" / "run_tests.sh").read_text(encoding="utf-8")
    local_run = (PROJECT_ROOT / "packaging" / "build-local-run.sh").read_text(encoding="utf-8")
    source_zip = (PROJECT_ROOT / "packaging" / "build-source-zip.sh").read_text(encoding="utf-8")
    cmake = (PROJECT_ROOT / "cmake" / "project.cmake").read_text(encoding="utf-8")
    native_quarantine = (
        PROJECT_ROOT / "tests" / "test_native_write_quarantine.cmake"
    ).read_text(encoding="utf-8")
    design = (PROJECT_ROOT / "docs" / "DESIGN.md").read_text(encoding="utf-8")
    gitignore = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

    for required in (
        "workflow_call:",
        "pull_request:",
        "LD_ENABLE_WERROR=ON",
        "git ls-files",
        "ctest --test-dir build --output-on-failure",
        "tests/test_btrfs_architecture.py",
        "tests/test_release_gate.py",
    ):
        assert required in gate, f"quality gate lost required check: {required}"
    assert "-E '^linux-defragger-tests$'" not in gate, (
        "quality gate must not exclude the aggregate project test harness"
    )
    for required in (
        "linux-defragger-native-write-quarantine",
        "test_native_write_quarantine.cmake",
    ):
        assert required in cmake, f"CMake lost executable quarantine coverage: {required}"
    for required in (
        "LD_FAT_WORKER",
        "LD_EXT_WORKER",
        "LD_NTFS_WORKER",
        "LD_EXFAT_WORKER",
        "LD_XFS_WORKER",
        "LD_AFFS_WORKER",
        "LD_HFSPLUS_WORKER",
        "LINUX_DEFRAGGER_ENABLE_UNAUDITED_WRITES",
        "quarantined",
    ):
        assert required in native_quarantine, (
            f"native executable quarantine test lost coverage: {required}"
        )

    for required in (
        "tests/test_release_artifacts.sh",
        "tests/run_typecheck.sh",
        "tests/test_spdx_licensing.py",
        "tests/test_no_external_fs_tools.py",
        "tests/test_architecture.py",
        "tests/test_gui_models.py",
        "tests/test_gui_services.py",
        "tests/test_transactions.py",
        "tests/test_safety.py",
        "tests/test_write_quarantine.py",
        "tests/test_xfs_writer.py",
        "verify_defragged_image.py",
        "verify_growth_defrag.py",
        "verify_growth_fat12_16.py",
    ):
        assert required in harness, f"aggregate harness lost required regression: {required}"

    trigger_block = release.split("permissions:", 1)[0]
    assert "workflow_dispatch:" in trigger_block
    assert "\n  push:" not in trigger_block, (
        "release publication must be an explicit manual dispatch, not a root marker push"
    )
    assert ".release-request" not in release, (
        "legacy root release marker must not be part of the release contract"
    )
    for required in (
        "quarantine-notice:",
        "if: ${{ false }}",
        "AUDIT_STATUS.md",
        "quality-gate:",
        "uses: ./.github/workflows/quality-gate.yml",
        "needs: quality-gate",
        'refs/heads/main',
        "packaging/build-source-zip.sh",
        'Defragger-${VERSION}.zip',
        "RELEASE_SHA256SUMS.txt",
        "gh release create",
    ):
        assert required in release, f"release workflow lost required contract: {required}"
    assert release.index("needs: quality-gate") < release.index("gh release create")
    assert "GitHub-generated source archive" not in release

    assert "__pycache__" in source_zip and ".pyc" in source_zip
    assert "__pycache__/" in gitignore
    assert "*.py[cod]" in gitignore

    common_contract = (
        ('COMMON_TAG="v1.6.0"', 'INFILTRATR_COMMON_TAG "v1.6.0"'),
        ('COMMON_VERSION="1.6.0"', 'INFILTRATR_COMMON_EXPECTED_VERSION "1.6.0"'),
        (
            'COMMON_COMMIT="7dc1195efd3f066e84c57520b44b2aa448847b90"',
            '7dc1195efd3f066e84c57520b44b2aa448847b90',
        ),
    )
    for local_required, cmake_required in common_contract:
        assert local_required in local_run, (
            f"local installer lost Common release contract: {local_required}"
        )
        assert cmake_required in cmake, (
            f"CMake lost matching Common release contract: {cmake_required}"
        )
    assert "v1.5.0" not in local_run
    assert 'COMMON_VERSION="1.5.0"' not in local_run
    assert "Infiltratr Common 1.6.0" in design
    assert "7dc1195efd3f066e84c57520b44b2aa448847b90" in design
    assert "Infiltratr Common 1.5.0" not in design

    print("release quality-gate contract passed")


if __name__ == "__main__":
    main()
