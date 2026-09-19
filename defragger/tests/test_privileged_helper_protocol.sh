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
grep -q '"type":"pong"' <<<"$output"
grep -q '"id":7' <<<"$output"
grep -q '"type":"bye"' <<<"$output"

python3 - "$HELPER" <<'PY'
import os
import subprocess
import sys

helper = sys.argv[1]
read_fd, write_fd = os.pipe()
os.close(read_fd)
process = subprocess.Popen(
    [helper],
    stdin=subprocess.PIPE,
    stdout=write_fd,
    stderr=subprocess.PIPE,
    text=True,
)
os.close(write_fd)
_, error = process.communicate('{"action":"ping","id":9}\n', timeout=5)
if process.returncode != 1:
    raise SystemExit(
        f"helper did not fail safely after protocol output closed; "
        f"rc={process.returncode} stderr={error!r}"
    )
PY

printf '%s\n' 'native privileged-helper protocol and closed-pipe safety passed'
