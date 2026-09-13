#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Root-side updater with an atomic verified-file handoff.

The GTK process downloads into a user-owned temporary directory.  This helper
is installed root-owned and is invoked through pkexec.  It opens that exact
path without following links, copies and hashes the bytes into a root-owned
temporary file in one pass, and only then invokes the fixed installer.  The
installer never receives the user-owned pathname, so a post-download pathname
replacement cannot turn a previously verified download into a different root
installation.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_DOWNLOAD_DIR_RE = re.compile(r"^linux-defragger-update-[A-Za-z0-9_.-]+$")
_ASSET_RE = re.compile(
    r"^(?:linux-defragger_[0-9]+\.[0-9]+\.[0-9]+-[0-9]+_amd64\.deb|"
    r"linux-defragger-[0-9]+\.[0-9]+\.[0-9]+-[0-9]+-local-folder\.run)$"
)


class UpdateHelperError(RuntimeError):
    """The requested update did not satisfy the root-side trust policy."""


def _caller_uid() -> int:
    value = os.environ.get("PKEXEC_UID", "")
    if not value.isdecimal():
        raise UpdateHelperError("pkexec did not identify the invoking user")
    return int(value, 10)


def _open_download(path: Path, caller_uid: int) -> int:
    if not path.is_absolute() or not _DOWNLOAD_DIR_RE.fullmatch(path.parent.name):
        raise UpdateHelperError("update source is not in the updater download area")
    if not _ASSET_RE.fullmatch(path.name):
        raise UpdateHelperError("update source filename is not a published asset")
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise UpdateHelperError(f"cannot open update source: {exc}") from exc
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise UpdateHelperError("update source is not a regular file")
        if info.st_uid != caller_uid:
            raise UpdateHelperError("update source is not owned by the invoking user")
        if info.st_mode & 0o022:
            raise UpdateHelperError("update source is writable by another account")
        return fd
    except Exception:
        os.close(fd)
        raise


def _copy_verified(source_fd: int, destination: Path, expected: str) -> None:
    digest = hashlib.sha256()
    with os.fdopen(source_fd, "rb", closefd=True) as source, destination.open("wb") as output:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            output.write(chunk)
        output.flush()
        os.fsync(output.fileno())
    actual = digest.hexdigest()
    if actual != expected.lower():
        raise UpdateHelperError("update bytes changed or failed root-side digest verification")


def install(kind: str, source: Path, expected: str) -> int:
    if kind not in {"deb", "run"}:
        raise UpdateHelperError("unknown update installer kind")
    if not _SHA256_RE.fullmatch(expected):
        raise UpdateHelperError("invalid expected update digest")
    caller_uid = _caller_uid()
    source_fd = _open_download(source, caller_uid)
    stage_dir = Path(tempfile.mkdtemp(prefix="linux-defragger-install-", dir="/var/tmp"))
    try:
        staged = stage_dir / source.name
        os.chmod(stage_dir, 0o700)
        _copy_verified(source_fd, staged, expected)
        os.chmod(staged, 0o700 if kind == "run" else 0o600)
        if kind == "run":
            command = [str(staged)]
        else:
            command = ["/usr/bin/apt-get", "install", "-y", str(staged)]
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, check=False)
        return completed.returncode
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


def main(argv: list[str]) -> int:
    if os.geteuid() != 0 or len(argv) != 4:
        print("usage: update_helper.py deb|run DOWNLOAD EXPECTED_SHA256", file=sys.stderr)
        return 2
    try:
        return install(argv[1], Path(argv[2]), argv[3])
    except UpdateHelperError as exc:
        print(f"Linux Defragger update refused: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
