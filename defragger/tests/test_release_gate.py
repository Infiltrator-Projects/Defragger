#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ensure release publication cannot bypass main-only CI or the completed audit."""

from __future__ import annotations

import ast
import re
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
    design = (PROJECT_ROOT / "docs" / "DESIGN.md").read_text(encoding="utf-8")
    audit = (PROJECT_ROOT / "docs" / "AUDIT_STATUS.md").read_text(encoding="utf-8")
    architecture = (PROJECT_ROOT / "tests" / "test_architecture.py").read_text(encoding="utf-8")
    version = (PROJECT_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    gitignore = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

    for required in (
        "workflow_call:",
        "push:",
        "- main",
        "LD_ENABLE_WERROR=ON",
        "git ls-files",
        "ctest --test-dir build --output-on-failure",
        "tests/test_btrfs_architecture.py",
        "tests/test_release_gate.py",
    ):
        assert required in gate, f"quality gate lost required check: {required}"
    assert "pull_request:" not in gate, "quality gate must not require PR branches"
    assert "merge_group:" not in gate, "quality gate must not require merge branches"
    assert '"c-first-*"' not in gate, "quality gate must run from main only"
    assert "-E '^linux-defragger-tests$'" not in gate, (
        "quality gate must not exclude the aggregate project test harness"
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
        "tests/test_native_top3.py",
        "tests/test_affs_native.py",
        "tests/test_hfsplus_native.py",
        "tests/test_xfs_writer.py",
        "verify_defragged_image.py",
        "verify_growth_defrag.py",
        "verify_growth_fat12_16.py",
    ):
        assert required in harness, f"aggregate harness lost required regression: {required}"

    trigger_block = release.split("permissions:", 1)[0]
    assert "workflow_run:" in trigger_block
    assert 'workflows: ["Project quality gate"]' in trigger_block
    assert "workflow_dispatch:" not in trigger_block, (
        "release publication must not require manual approval"
    )
    assert "\n  push:" not in trigger_block, (
        "release publication must wait for the completed main quality gate"
    )
    assert ".release-request" not in release, (
        "legacy root release marker must not be part of the release contract"
    )
    for required in (
        "github.event.workflow_run.conclusion == 'success'",
        "github.event.workflow_run.event == 'push'",
        "github.event.workflow_run.head_branch == 'main'",
        "startsWith(github.event.workflow_run.head_commit.message, 'Release ')",
        "Verify permanent main quality-gate protection",
        '"repos/${GITHUB_REPOSITORY}/rules/branches/main"',
        '"required_status_checks"',
        '"quality-gate"',
        "EXPECTED_SHA",
        "origin/main",
        "packaging/build-source-zip.sh",
        'Defragger-${VERSION}.zip',
        "RELEASE_SHA256SUMS.txt",
        "Status: **complete**",
        'Applies to: release version ${VERSION}',
        "published releases are immutable",
        "gh release create",
    ):
        assert required in release, f"release workflow lost required contract: {required}"
    assert "quarantine-notice:" not in release
    assert "if: ${{ false }}" not in release
    assert "--clobber" not in release
    assert "gh release edit" not in release
    assert "GitHub-generated source archive" not in release

    assert "__pycache__" in source_zip and ".pyc" in source_zip
    assert "__pycache__/" in gitignore
    assert "*.py[cod]" in gitignore

    common_contract = (
        ('COMMON_TAG="v1.15.4"', 'INFILTRATR_COMMON_TAG "v1.15.4"'),
        ('COMMON_VERSION="1.15.4"', 'INFILTRATR_COMMON_EXPECTED_VERSION "1.15.4"'),
        (
            'COMMON_COMMIT="046406bea2aefa539c74e1038b6c20825eca8af7"',
            '046406bea2aefa539c74e1038b6c20825eca8af7',
        ),
    )
    for local_required, cmake_required in common_contract:
        assert local_required in local_run, (
            f"local installer lost Common release contract: {local_required}"
        )
        assert cmake_required in cmake, (
            f"CMake lost matching Common release contract: {cmake_required}"
        )
    for stale_version in ("1.5.0", "1.6.0"):
        assert f"v{stale_version}" not in local_run
        assert f'COMMON_VERSION="{stale_version}"' not in local_run
        assert f"Infiltratr Common {stale_version}" not in design
    assert "Infiltratr Common 1.15.4" in design
    assert "046406bea2aefa539c74e1038b6c20825eca8af7" in design

    for required in (
        "Status: **complete**",
        "FAT12/FAT16/FAT32",
        "EXT2/EXT3/EXT4",
        "NTFS",
        "exFAT",
        "XFS",
        "Amiga OFS/FFS",
        "Amiga SFS0",
        "HFS+/HFSX",
        "Project quality gate",
        "protected main",
        "explicit release decision",
    ):
        assert required in audit, f"completed safety audit lost evidence: {required}"

    assert f"Applies to: release version {version}" in audit, (
        "completed safety audit does not apply to the current VERSION"
    )

    writer_assignment = None
    for node in ast.parse(architecture).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "NATIVE_WRITERS"
            for target in node.targets
        ):
            writer_assignment = ast.literal_eval(node.value)
            break
    assert isinstance(writer_assignment, dict), (
        "cannot determine authoritative native writer set from architecture test"
    )
    match = re.search(r"^Audited writer IDs:\\s*(.+)$", audit, re.MULTILINE)
    assert match is not None, "completed safety audit lost Audited writer IDs"
    audited_writer_ids = {
        item.strip() for item in match.group(1).split(",") if item.strip()
    }
    expected_writer_ids = set(writer_assignment)
    assert audited_writer_ids == expected_writer_ids, (
        "safety audit writer scope does not match enabled native writers: "
        f"audited={sorted(audited_writer_ids)} enabled={sorted(expected_writer_ids)}"
    )

    print("release quality-gate contract passed")


if __name__ == "__main__":
    main()
