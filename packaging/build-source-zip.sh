#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Create the complete clean source release archive using GitHub-compatible naming.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
PARENT=$(dirname -- "$ROOT")
ARCHIVE_BASENAME="Defragger-${VERSION}"
OUTPUT=${1:-"$PARENT/${ARCHIVE_BASENAME}.zip"}

command -v zip >/dev/null 2>&1 || {
    printf '%s\n' 'zip is required to create the source archive.' >&2
    exit 1
}

OUTPUT_DIRECTORY=$(dirname -- "$OUTPUT")
[ -d "$OUTPUT_DIRECTORY" ] || mkdir -p -- "$OUTPUT_DIRECTORY"
case "$OUTPUT" in
    /*) ;;
    *) OUTPUT=$(CDPATH= cd -- "$(dirname -- "$OUTPUT")" && pwd)/$(basename -- "$OUTPUT") ;;
esac

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-source-zip.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
ln -s "$ROOT" "$STAGE/$ARCHIVE_BASENAME"

(
    cd "$STAGE"
    zip -X -q -9 -r "$OUTPUT" "$ARCHIVE_BASENAME" \
        -x "$ARCHIVE_BASENAME/.git/*" \
           "$ARCHIVE_BASENAME/__pycache__/*" \
           "$ARCHIVE_BASENAME/*/__pycache__/*" \
           "$ARCHIVE_BASENAME/*/*/__pycache__/*" \
           "$ARCHIVE_BASENAME/*/*/*/__pycache__/*" \
           "$ARCHIVE_BASENAME/*.pyc" "$ARCHIVE_BASENAME/*/*.pyc" \
           "$ARCHIVE_BASENAME/*/*/*.pyc" "$ARCHIVE_BASENAME/*/*/*/*.pyc" \
           "$ARCHIVE_BASENAME/build*" "$ARCHIVE_BASENAME/build*/*" \
           "$ARCHIVE_BASENAME/native-verify/*" \
           "$ARCHIVE_BASENAME/release/*" \
           "$ARCHIVE_BASENAME/*.deb" "$ARCHIVE_BASENAME/*.run" "$ARCHIVE_BASENAME/*.zip"
)

printf '%s\n' "$OUTPUT"
