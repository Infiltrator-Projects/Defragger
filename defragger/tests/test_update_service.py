#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for the GUI-first Linux Defragger update path."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.update_service import (
    UpdateError,
    checksum_from_manifest,
    installer_command,
    release_from_payload,
    version_key,
)
from ui.update_service import UPDATE_HELPER


def _release_payload(version: str) -> dict:
    return {
        "tag_name": f"v{version}",
        "draft": False,
        "prerelease": False,
        "assets": [
            {
                "name": f"linux-defragger_{version}_amd64.deb",
                "state": "uploaded",
                "browser_download_url": f"https://example.invalid/generic-{version}.deb",
                "digest": "sha256:" + ("a" * 64),
            },
            {
                "name": f"linux-defragger-{version}-local-folder.run",
                "state": "uploaded",
                "browser_download_url": f"https://example.invalid/native-{version}.run",
                "digest": "sha256:" + ("b" * 64),
            },
            {
                "name": "RELEASE_SHA256SUMS.txt",
                "state": "uploaded",
                "browser_download_url": "https://example.invalid/SHA256SUMS",
                "digest": "sha256:" + ("c" * 64),
            },
        ],
    }


def test_release_selection_preserves_build_profile() -> None:
    payload = _release_payload("1.8.0-145")
    generic = release_from_payload(payload, "1.8.0-144", "generic")
    assert generic is not None
    assert generic.installer_kind == "deb"
    assert generic.asset_name == "linux-defragger_1.8.0-145_amd64.deb"

    native = release_from_payload(payload, "1.8.0-144", "native")
    assert native is not None
    assert native.installer_kind == "run"
    assert native.asset_name == "linux-defragger-1.8.0-145-local-folder.run"

    assert release_from_payload(payload, "1.8.0-145", "generic") is None
    assert version_key("1.8.0-145") > version_key("1.8.0-144")


def test_release_selection_rejects_missing_or_invalid_assets() -> None:
    payload = _release_payload("1.8.0-145")
    payload["assets"][0]["digest"] = "sha256:not-a-digest"
    try:
        release_from_payload(payload, "1.8.0-144", "generic")
    except UpdateError:
        pass
    else:
        raise AssertionError("invalid GitHub asset digest was accepted")

    payload = _release_payload("1.8.0-145")
    payload["assets"] = [
        asset for asset in payload["assets"]
        if asset["name"] != "RELEASE_SHA256SUMS.txt"
    ]
    try:
        release_from_payload(payload, "1.8.0-144", "generic")
    except UpdateError:
        pass
    else:
        raise AssertionError("release without SHA-256 manifest was accepted")


def test_checksum_manifest_uses_exact_asset_name() -> None:
    digest = "d" * 64
    manifest = (
        f"{'e' * 64}  linux-defragger_1.8.0-144_amd64.deb\n"
        f"{digest}  linux-defragger_1.8.0-145_amd64.deb\n"
    )
    assert checksum_from_manifest(
        manifest, "linux-defragger_1.8.0-145_amd64.deb"
    ) == digest


def test_installer_commands_never_use_a_shell() -> None:
    payload = _release_payload("1.8.0-145")
    generic = release_from_payload(payload, "1.8.0-144", "generic")
    native = release_from_payload(payload, "1.8.0-144", "native")
    assert generic is not None and native is not None

    deb = Path("/tmp/linux-defragger_1.8.0-145_amd64.deb")
    run = Path("/tmp/linux-defragger-1.8.0-145-local-folder.run")
    digest = "a" * 64
    assert installer_command(generic, deb, digest) == (
        "/usr/bin/pkexec",
        UPDATE_HELPER,
        "deb",
        str(deb),
        digest,
    )
    assert installer_command(native, run, "b" * 64) == (
        "/usr/bin/pkexec",
        UPDATE_HELPER,
        "run",
        str(run),
        "b" * 64,
    )


def test_root_update_helper_is_packaged_and_has_fixed_installers() -> None:
    helper = (ROOT / "gui" / "update_helper.py").read_text()
    package_builder = (ROOT / "cmake" / "project.cmake").read_text()
    assert "O_NOFOLLOW" in helper
    assert "os.fstat" in helper
    assert "os.fsync" in helper
    assert '"/usr/bin/apt-get", "install", "-y"' in helper
    assert "subprocess.run(command" in helper
    assert "gui/update_helper.py" in package_builder


def test_desktop_launchers_are_absolute_and_update_aware() -> None:
    desktop = (ROOT / "packaging" / "io.github.linuxdefragger.desktop").read_text()
    test_media = (
        ROOT / "packaging" / "io.github.linuxdefragger.TestMedia.desktop"
    ).read_text()
    wrapper = (ROOT / "packaging" / "linux-defragger").read_text()
    package_builder = (ROOT / "packaging" / "build-deb.sh").read_text()
    local_runner = (ROOT / "packaging" / "local-run-header.sh.in").read_text()

    assert "Exec=/usr/bin/linux-defragger" in desktop
    assert "TryExec=/usr/bin/linux-defragger" in desktop
    assert "Exec=/usr/bin/linux-defragger --check-updates" in desktop
    assert "TryExec=/usr/bin/linux-defragger-test-media" in test_media
    assert "Exec=/usr/bin/linux-defragger-test-media" in test_media
    assert "/usr/lib/linux-defragger/ui/update_launcher.py" in wrapper
    assert "update-desktop-database /usr/share/applications" in package_builder
    assert "A later Linux Defragger release can upgrade it normally through APT." not in local_runner


if __name__ == "__main__":
    test_release_selection_preserves_build_profile()
    test_release_selection_rejects_missing_or_invalid_assets()
    test_checksum_manifest_uses_exact_asset_name()
    test_installer_commands_never_use_a_shell()
    test_root_update_helper_is_packaged_and_has_fixed_installers()
    test_desktop_launchers_are_absolute_and_update_aware()
    print("Linux Defragger update-service tests passed")
