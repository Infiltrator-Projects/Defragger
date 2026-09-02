#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Install a completed local build through the canonical CMake manifest.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD=${BUILD_DIR:-"$ROOT/build"}

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Linux Defragger install.sh must be run as root (for example: sudo ./install.sh).' >&2
    exit 1
fi

command -v cmake >/dev/null 2>&1 || {
    printf '%s\n' 'cmake is required to install Linux Defragger.' >&2
    exit 1
}

[ -f "$BUILD/cmake_install.cmake" ] || {
    printf 'No completed CMake build was found at %s. Build Linux Defragger first.\n' "$BUILD" >&2
    exit 1
}

# CMake owns the complete installed-file inventory. Keeping one install manifest
# prevents this compatibility helper from drifting behind new filesystem workers,
# launchers, desktop files or documentation.
cmake --install "$BUILD" --prefix /usr
