#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Create the complete clean local-source release archive.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
PARENT=$(dirname -- "$ROOT")
BASENAME=$(basename -- "$ROOT")
OUTPUT=${1:-"$PARENT/linux-defragger-${VERSION}-local-source.zip"}

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

(
    cd "$PARENT"
    zip -X -q -9 -r "$OUTPUT" "$BASENAME" \
        -x "$BASENAME/.git/*" \
           "$BASENAME/__pycache__/*" \
           "$BASENAME/*/__pycache__/*" \
           "$BASENAME/*/*/__pycache__/*" \
           "$BASENAME/*/*/*/__pycache__/*" \
           "$BASENAME/*.pyc" "$BASENAME/*/*.pyc" \
           "$BASENAME/*/*/*.pyc" "$BASENAME/*/*/*/*.pyc" \
           "$BASENAME/build*" "$BASENAME/build*/*" \
           "$BASENAME/native-verify/*" \
           "$BASENAME/release/*" \
           "$BASENAME/*.deb" "$BASENAME/*.run" "$BASENAME/*.zip"
)

printf '%s\n' "$OUTPUT"
