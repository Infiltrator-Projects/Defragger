#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

if command -v pyright >/dev/null 2>&1; then
    exec pyright --project tests/pyrightconfig.json
fi
if command -v npx >/dev/null 2>&1; then
    exec npx --yes pyright@1.1.403 --project tests/pyrightconfig.json
fi

echo "pyright or npx is required for the release type-check gate" >&2
exit 1
