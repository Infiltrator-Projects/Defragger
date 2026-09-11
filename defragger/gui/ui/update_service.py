#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Secure GitHub-release discovery, download and installation for Linux Defragger."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Any
from urllib.request import Request, urlopen

LATEST_RELEASE_API = (
    "https://api.github.com/repos/Infiltrator-Projects/Defragger/releases/latest"
)
USER_AGENT = "Linux-Defragger-Updater"
_VERSION_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)-(\d+)$")
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class UpdateError(RuntimeError):
    """A release could not be trusted, downloaded or installed."""


@dataclass(frozen=True)
class UpdateRelease:
    version: str
    tag: str
    asset_name: str
    asset_url: str
    asset_digest: str | None
    checksum_url: str
    checksum_digest: str | None
    installer_kind: str


def version_key(value: str) -> tuple[int, int, int, int]:
    match = _VERSION_RE.fullmatch(value.strip())
    if match is None:
        raise UpdateError(f"Unsupported Linux Defragger version: {value!r}")
    return tuple(int(part) for part in match.groups())


def _asset(payload: dict[str, Any], name: str) -> dict[str, Any]:
    for asset in payload.get("assets", []):
        if asset.get("name") == name and asset.get("state") == "uploaded":
            return asset
    raise UpdateError(f"Published release is missing required asset {name}.")


def _asset_digest(asset: dict[str, Any], name: str) -> str | None:
    digest = asset.get("digest")
    if digest is None:
        return None
    digest = str(digest)
    if not digest.startswith("sha256:") or not _SHA256_RE.fullmatch(digest[7:]):
        raise UpdateError(f"GitHub reported an invalid digest for {name}.")
    return digest[7:].lower()


def release_from_payload(
    payload: dict[str, Any],
    current_version: str,
    build_profile: str,
) -> UpdateRelease | None:
    if payload.get("draft") or payload.get("prerelease"):
        return None

    tag = str(payload.get("tag_name") or "")
    latest_version = tag[1:] if tag.startswith("v") else tag
    if version_key(latest_version) <= version_key(current_version):
        return None

    if build_profile == "native":
        asset_name = f"linux-defragger-{latest_version}-local-folder.run"
        installer_kind = "run"
    else:
        asset_name = f"linux-defragger_{latest_version}_amd64.deb"
        installer_kind = "deb"

    package_asset = _asset(payload, asset_name)
    checksum_asset = _asset(payload, "RELEASE_SHA256SUMS.txt")

    return UpdateRelease(
        version=latest_version,
        tag=tag,
        asset_name=asset_name,
        asset_url=str(package_asset["browser_download_url"]),
        asset_digest=_asset_digest(package_asset, asset_name),
        checksum_url=str(checksum_asset["browser_download_url"]),
        checksum_digest=_asset_digest(checksum_asset, "RELEASE_SHA256SUMS.txt"),
        installer_kind=installer_kind,
    )


def check_latest(current_version: str, build_profile: str) -> UpdateRelease | None:
    request = Request(
        LATEST_RELEASE_API,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": f"{USER_AGENT}/{current_version}",
        },
    )
    try:
        with urlopen(request, timeout=6) as response:
            payload = json.load(response)
    except Exception as exc:
        raise UpdateError(
            f"Could not contact the Linux Defragger release service: {exc}"
        ) from exc
    if not isinstance(payload, dict):
        raise UpdateError("GitHub returned an invalid release response.")
    return release_from_payload(payload, current_version, build_profile)


def checksum_from_manifest(text: str, asset_name: str) -> str:
    for raw_line in text.splitlines():
        parts = raw_line.strip().split()
        if len(parts) < 2:
            continue
        candidate = parts[-1].lstrip("*")
        digest = parts[0].lower()
        if candidate == asset_name and _SHA256_RE.fullmatch(digest):
            return digest
    raise UpdateError(f"Checksum manifest does not contain {asset_name}.")


def _download(url: str, destination: Path, *, user_agent: str) -> None:
    request = Request(url, headers={"User-Agent": user_agent})
    try:
        with urlopen(request, timeout=30) as response, destination.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
    except Exception as exc:
        raise UpdateError(f"Download failed: {exc}") from exc


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def download_update(release: UpdateRelease, current_version: str) -> Path:
    directory = Path(tempfile.mkdtemp(prefix="linux-defragger-update-"))
    package_path = directory / release.asset_name
    checksum_path = directory / "RELEASE_SHA256SUMS.txt"
    agent = f"{USER_AGENT}/{current_version}"
    try:
        _download(release.checksum_url, checksum_path, user_agent=agent)
        manifest_digest = _sha256_file(checksum_path)
        if (
            release.checksum_digest is not None
            and manifest_digest != release.checksum_digest
        ):
            raise UpdateError(
                "GitHub checksum-manifest digest mismatch; refusing to install."
            )

        expected = checksum_from_manifest(
            checksum_path.read_text(encoding="utf-8", errors="strict"),
            release.asset_name,
        )

        _download(release.asset_url, package_path, user_agent=agent)
        actual = _sha256_file(package_path)
        if actual != expected:
            raise UpdateError(
                f"SHA-256 mismatch for {release.asset_name}; refusing to install it."
            )
        if release.asset_digest is not None and actual != release.asset_digest:
            raise UpdateError(
                f"GitHub asset digest mismatch for {release.asset_name}; "
                "refusing to install it."
            )
        if release.installer_kind == "run":
            package_path.chmod(0o700)
        return package_path
    except Exception:
        shutil.rmtree(directory, ignore_errors=True)
        raise


def installer_command(release: UpdateRelease, package_path: Path) -> tuple[str, ...]:
    if release.installer_kind == "run":
        return ("/usr/bin/pkexec", str(package_path))
    if release.installer_kind == "deb":
        return (
            "/usr/bin/pkexec",
            "/usr/bin/apt-get",
            "install",
            "-y",
            str(package_path),
        )
    raise UpdateError(f"Unknown update installer kind: {release.installer_kind}")


def install_update(release: UpdateRelease, package_path: Path) -> None:
    command = installer_command(release, package_path)
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=3600,
            check=False,
        )
    except Exception as exc:
        raise UpdateError(f"Could not start the privileged installer: {exc}") from exc
    if completed.returncode != 0:
        detail = (completed.stdout or "").strip()
        if completed.returncode in (126, 127):
            detail = detail or "Administrator authentication was cancelled."
        raise UpdateError(
            detail or f"Installer exited with status {completed.returncode}."
        )


def cleanup_download(package_path: Path | None) -> None:
    if package_path is None:
        return
    parent = package_path.parent
    if parent.name.startswith("linux-defragger-update-"):
        shutil.rmtree(parent, ignore_errors=True)
