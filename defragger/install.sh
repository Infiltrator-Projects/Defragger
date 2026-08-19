#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Install a completed local build and its modular runtime.

set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME=/usr/lib/linux-defragger

for entry in linux_defragger_gui.py allocation_mapper.py privileged_helper.py \
             operation_engine.py; do
    install -Dm755 "$ROOT/gui/$entry" "$RUNTIME/$entry"
done
install -Dm644 "$ROOT/build/generated/version.py" "$RUNTIME/version.py"

for package in core engine ui backends filesystems; do
    rm -rf "$RUNTIME/$package"
    mkdir -p "$RUNTIME/$package"
    cp -a "$ROOT/gui/$package/." "$RUNTIME/$package/"
done
find "$RUNTIME" -type d -name __pycache__ -prune -exec rm -rf {} +
find "$RUNTIME" -type f -name '*.pyc' -delete
find "$RUNTIME/filesystems" -type f -name 'writer.py' -exec chmod 755 {} +

# Native helpers belong to the filesystem packages. Install them after the
# Python package copy so the directory refresh above cannot remove them.
install -Dm755 "$ROOT/build/linux-defragger-fat-worker" \
    "$RUNTIME/filesystems/fat/linux-defragger-fat-worker"
install -Dm755 "$ROOT/build/linux-defragger-xfs-worker" \
    "$RUNTIME/filesystems/xfs/linux-defragger-xfs-worker"
install -Dm755 "$ROOT/build/linux-defragger-ext-worker" \
    "$RUNTIME/filesystems/ext4/linux-defragger-ext-worker"
install -Dm755 "$ROOT/build/linux-defragger-ntfs-worker" \
    "$RUNTIME/filesystems/ntfs/linux-defragger-ntfs-worker"
install -Dm755 "$ROOT/build/linux-defragger-exfat-worker" \
    "$RUNTIME/filesystems/exfat/linux-defragger-exfat-worker"
install -Dm755 "$ROOT/build/linux-defragger-affs-worker" \
    "$RUNTIME/filesystems/affs/linux-defragger-affs-worker"
install -Dm755 "$ROOT/build/hfs_analyser" \
    "$RUNTIME/filesystems/hfs/hfs_analyser"

install -Dm755 "$ROOT/tools/linux-defragger-testdata.py" /usr/bin/linux-defragger-testdata
install -Dm755 "$ROOT/packaging/linux-defragger" /usr/bin/linux-defragger
install -Dm644 "$ROOT/packaging/io.github.linuxdefragger.desktop" \
    /usr/share/applications/io.github.linuxdefragger.desktop
install -Dm644 "$ROOT/packaging/io.github.linuxdefragger.svg" \
    /usr/share/icons/hicolor/scalable/apps/io.github.linuxdefragger.svg

