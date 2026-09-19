#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch and verify the canonical MB Corpo bundle declared by pinned Common.
set -eu

DEST=${1:?usage: vendor-mb-fonts.sh DESTINATION_ARCHIVE}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

exec cmake \
    -DLD_ROOT="$ROOT" \
    -DLD_FONT_ARCHIVE="$DEST" \
    -P "$ROOT/packaging/vendor-mb-fonts.cmake"
