#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and verify the exact MB Corpo bundle described by pinned Common.
set -eu

DEST=${1:?usage: vendor-mb-fonts.sh DESTINATION_ARCHIVE}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DESIGN="$ROOT/shared/infiltratr-common/design/infiltrator-design-v1.json"

[ -f "$DESIGN" ] || {
    printf 'Pinned Common design contract is missing: %s\n' "$DESIGN" >&2
    exit 1
}

python3 - "$DESIGN" "$DEST" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import urllib.request

design_path = Path(sys.argv[1])
destination = Path(sys.argv[2])
design = json.loads(design_path.read_text(encoding="utf-8"))
typography = design["typography"]
assets = typography["assets"]
files = typography["font_files"]
hashes = assets["file_sha256"]

repository = assets["source_repository"]
commit = assets["source_commit"]
archive_path = assets["archive_path"]
url = f"https://raw.githubusercontent.com/{repository}/{commit}/{archive_path}"
expected_archive = assets["archive_sha256"].lower()

destination.parent.mkdir(parents=True, exist_ok=True)
if not destination.exists():
    temporary = destination.with_name(destination.name + ".tmp")
    try:
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)

actual_archive = hashlib.sha256(destination.read_bytes()).hexdigest()
if actual_archive != expected_archive:
    raise SystemExit(
        f"MB Corpo archive hash mismatch: {actual_archive} "
        f"(Common requires {expected_archive})"
    )

roles = (
    ("brand_regular", files["brand_regular"]),
    ("ui_bold", files["ui_bold"]),
    ("ui_regular", files["ui_regular"]),
)
with tempfile.TemporaryDirectory(prefix="linux-defragger-font-check.") as work:
    with tarfile.open(destination, mode="r:xz") as archive:
        archive.extractall(work)
    root = Path(work)
    for role, filename in roles:
        candidate = root / filename
        if not candidate.is_file():
            raise SystemExit(f"MB Corpo archive is missing {filename}")
        actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
        expected = hashes[role].lower()
        if actual != expected:
            raise SystemExit(
                f"MB Corpo hash mismatch for {filename}: {actual} "
                f"(Common requires {expected})"
            )

print(destination)
PY
