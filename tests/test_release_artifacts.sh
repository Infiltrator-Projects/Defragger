#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-artifact-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
RUN="$WORK/linux-defragger-${VERSION}-local-folder.run"
SOURCE_ZIP="$WORK/Defragger-${VERSION}.zip"

sh -n "$ROOT/packaging/build-deb.sh"
sh -n "$ROOT/packaging/build-local-run.sh"
sh -n "$ROOT/packaging/build-source-zip.sh"
"$ROOT/packaging/build-local-run.sh" "$RUN" >/dev/null
"$ROOT/packaging/build-source-zip.sh" "$SOURCE_ZIP" >/dev/null
[ -f "$SOURCE_ZIP" ]
unzip -Z1 "$SOURCE_ZIP" >"$WORK/source-files.txt"
grep -qx "Defragger-${VERSION}/CMakeLists.txt" "$WORK/source-files.txt"
grep -qx "Defragger-${VERSION}/packaging/build-source-zip.sh" "$WORK/source-files.txt"
if grep -Eq '(^|/)linux-defragger-[^/]*-local-source\.zip$' "$WORK/source-files.txt"; then
    printf '%s\n' 'Source archive contains the obsolete local-source ZIP name.' >&2
    exit 1
fi
grep -Fq 'OUTPUT=${1:-"$PARENT/${ARCHIVE_BASENAME}.zip"}' "$ROOT/packaging/build-source-zip.sh"
grep -Fq 'ARCHIVE_BASENAME="Defragger-${VERSION}"' "$ROOT/packaging/build-source-zip.sh"

MARKER_LINE=$(grep -an '^__LINUX_DEFRAGGER_PAYLOAD_BELOW__$' "$RUN" | \
    head -1 | cut -d: -f1)
[ -n "$MARKER_LINE" ]
PAYLOAD_LINE=$((MARKER_LINE + 1))
HEADER="$WORK/header.sh"
head -n "$MARKER_LINE" "$RUN" >"$HEADER"
sh -n "$HEADER"

EXPECTED=$(sed -n "s/^PAYLOAD_SHA256='\([^']*\)'$/\1/p" "$HEADER")
ACTUAL=$(tail -n +"$PAYLOAD_LINE" "$RUN" | sha256sum | awk '{print $1}')
[ "$ACTUAL" = "$EXPECTED" ]

tail -n +"$PAYLOAD_LINE" "$RUN" | tar -tzf - >"$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/CMakeLists.txt" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/packaging/build-deb.sh" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/packaging/build-local-run.sh" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/packaging/build-source-zip.sh" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/THIRD_PARTY_NOTICES.md" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/LICENSES/GPL-3.0-or-later.txt" "$WORK/files.txt"
if grep -Eq '/(build[^/]*)/|__pycache__|\.pyc$|\.deb$|\.run$|\.zip$' "$WORK/files.txt"; then
    printf '%s\n' 'Generated local installer contains a forbidden generated file.' >&2
    exit 1
fi

dpkg --compare-versions "${VERSION}+native1" gt "$VERSION"
NEXT_REVISION=${VERSION%-*}-$(( ${VERSION##*-} + 1 ))
dpkg --compare-versions "$NEXT_REVISION" gt "${VERSION}+native1"

grep -q -- '-march=x86-64' "$ROOT/CMakeLists.txt"
grep -q -- '-mtune=generic' "$ROOT/CMakeLists.txt"
grep -q -- '-march=native' "$ROOT/CMakeLists.txt"
grep -q -- '-mtune=native' "$ROOT/CMakeLists.txt"
grep -Fq "printf 'Version: %s\\n' \"\$PACKAGE_VERSION\"" \
    "$ROOT/packaging/build-deb.sh"
if grep -Fq "printf 'Version: %s\\n' \"\$VERSION\"" \
    "$ROOT/packaging/build-deb.sh"; then
    printf '%s\n' 'Debian control metadata ignores DEB_PACKAGE_VERSION.' >&2
    exit 1
fi
"$ROOT/tests/test_deb_package_versions.sh"
"$ROOT/tests/test_local_installer_end_to_end.sh" "$RUN"

printf '%s\n' 'Three-file release packaging tests passed.'
