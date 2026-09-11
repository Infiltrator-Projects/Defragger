#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and verify the exact MB Corpo font bundle supplied to MBLINK.
set -eu

DEST=${1:?usage: vendor-mb-fonts.sh DESTINATION_ARCHIVE}
MBLINK_COMMIT=aa161e7342112beab8feb7669f072c870f742765
URL="https://raw.githubusercontent.com/Infiltrator-Projects/MBLINK/${MBLINK_COMMIT}/assets/fonts/mb-corpo-fonts.tar.xz"
ARCHIVE_SHA256=bdb6063f838a7fab22b4d6b412170640c69511df53aa3dfa9a4ea8431c9d8274
A_SHA256=c8bcd7e1a7d71169b38491d9b7c1ffe7ba7b46e888f0c1219931343a47bc0e05
S_BOLD_SHA256=d37ea986e2344d83390f94f170e6272b56efd00bfec808afe8314c4ca45d43b4
S_REGULAR_SHA256=94ede6629443c03d4362dcef425fb3ff520be5d654370021a34e81286804465c

mkdir -p "$(dirname -- "$DEST")"
if [ ! -f "$DEST" ]; then
    TMP="${DEST}.tmp.$$"
    trap 'rm -f "$TMP"' EXIT HUP INT TERM
    python3 - "$URL" "$TMP" <<'PY'
import sys
import urllib.request
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
PY
    mv "$TMP" "$DEST"
    trap - EXIT HUP INT TERM
fi

actual=$(sha256sum "$DEST" | awk '{print $1}')
[ "$actual" = "$ARCHIVE_SHA256" ] || {
    printf 'MB Corpo archive hash mismatch: %s\n' "$actual" >&2
    exit 1
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-font-check.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
tar -xJf "$DEST" -C "$WORK"
for file in mb_corpo_a_cond_regular.ttf mb_corpo_s_bold.ttf mb_corpo_s_regular.ttf; do
    [ -f "$WORK/$file" ] || {
        printf 'MB Corpo archive is missing %s\n' "$file" >&2
        exit 1
    }
done
[ "$(sha256sum "$WORK/mb_corpo_a_cond_regular.ttf" | awk '{print $1}')" = "$A_SHA256" ] || exit 1
[ "$(sha256sum "$WORK/mb_corpo_s_bold.ttf" | awk '{print $1}')" = "$S_BOLD_SHA256" ] || exit 1
[ "$(sha256sum "$WORK/mb_corpo_s_regular.ttf" | awk '{print $1}')" = "$S_REGULAR_SHA256" ] || exit 1
printf '%s\n' "$DEST"
