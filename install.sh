#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Thin repository-root launcher for the project installer.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$ROOT/defragger/install.sh" "$@"
