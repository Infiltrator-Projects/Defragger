#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Create the self-contained local hardware-optimised compiler/installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
NATIVE_PACKAGE_VERSION="${VERSION}+native1"
OUTPUT=${1:-"$ROOT/linux-defragger-${VERSION}-local-folder.run"}
TEMPLATE="$ROOT/packaging/local-run-header.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-run-build.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

for command_name in tar gzip sha256sum sed; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'Required command is missing: %s\n' "$command_name" >&2
        exit 1
    }
done

[ -f "$TEMPLATE" ] || {
    printf 'Installer template is missing: %s\n' "$TEMPLATE" >&2
    exit 1
}

PARENT=$(dirname -- "$ROOT")
BASENAME=$(basename -- "$ROOT")
TAR_PATH="$WORK/source.tar"

tar --sort=name --mtime='@1704067200' --owner=0 --group=0 --numeric-owner \
    --exclude-vcs \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude="$BASENAME/build*" \
    --exclude="$BASENAME/native-verify" \
    --exclude="$BASENAME/release" \
    --exclude='*.deb' \
    --exclude='*.run' \
    --exclude='*.zip' \
    -C "$PARENT" -cf "$TAR_PATH" "$BASENAME"
gzip -n -9 "$TAR_PATH"
PAYLOAD="$TAR_PATH.gz"

PAYLOAD_SHA256=$(sha256sum "$PAYLOAD" | awk '{print $1}')
umask 022
sed \
    -e "s/@APP_VERSION@/$VERSION/g" \
    -e "s/@NATIVE_PACKAGE_VERSION@/$NATIVE_PACKAGE_VERSION/g" \
    -e "s/@PAYLOAD_SHA256@/$PAYLOAD_SHA256/g" \
    "$TEMPLATE" >"$OUTPUT"
dd if="$PAYLOAD" of="$OUTPUT" oflag=append conv=notrunc status=none
chmod 0755 "$OUTPUT"

printf '%s\n' "$OUTPUT"
