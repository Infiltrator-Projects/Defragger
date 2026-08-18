#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ensure release publication cannot bypass the permanent project quality gate."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    gate = (ROOT / ".github" / "workflows" / "quality-gate.yml").read_text(encoding="utf-8")
    release = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")

    for required in (
        "workflow_call:",
        "pull_request:",
        "LD_ENABLE_WERROR=ON",
        "ctest --test-dir build --output-on-failure",
        "tests/test_spdx_licensing.py",
        "tests/test_no_external_fs_tools.py",
        "tests/test_architecture.py",
        "tests/test_btrfs_architecture.py",
        "tests/test_release_artifacts.sh",
    ):
        assert required in gate, f"quality gate lost required check: {required}"

    assert "quality-gate:" in release
    assert "uses: ./.github/workflows/quality-gate.yml" in release
    assert "needs: quality-gate" in release
    assert "- VERSION" in release
    assert "gh release create" in release
    assert release.index("needs: quality-gate") < release.index("gh release create")

    print("release quality-gate contract passed")


if __name__ == "__main__":
    main()
