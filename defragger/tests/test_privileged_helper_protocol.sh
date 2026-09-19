#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

HELPER=${1:?privileged helper test binary is required}

output=$(
    printf '%s\n' \
        '{"action":"ping","id":7}' \
        '{"action":"quit"}' | "$HELPER"
)
grep -q '"type":"ready"' <<<"$output"
grep -q '"type":"pong","id":7' <<<"$output"
grep -q '"type":"bye"' <<<"$output"

set +e
printf '%s\n' '{"action":"ping","id":9}' | "$HELPER" | head -n 1 >/dev/null
status=( "${PIPESTATUS[@]}" )
set -e
[ "${status[1]}" -eq 1 ] || {
    printf 'helper did not fail safely after protocol output closed; rc=%s\n' "${status[1]}" >&2
    exit 1
}

printf '%s\n' 'native privileged-helper protocol and closed-pipe safety passed'
