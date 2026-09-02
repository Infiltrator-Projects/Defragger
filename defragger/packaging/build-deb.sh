#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a Debian-managed generic or locally optimised package.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\n\r' <"$ROOT/VERSION")
PACKAGE_VERSION=${DEB_PACKAGE_VERSION:-$VERSION}
ARCH=${DEB_HOST_ARCH:-$(dpkg --print-architecture)}
BUILD_FLAVOR=${LD_BUILD_FLAVOR:-generic}
BUILD_TESTING=${LD_BUILD_TESTING:-ON}
BUILD=${BUILD_DIR:-"$ROOT/build-deb-$BUILD_FLAVOR"}
OUTPUT=${OUTPUT_PATH:-"$ROOT/linux-defragger_${PACKAGE_VERSION}_${ARCH}.deb"}
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-deb.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM

if [ "$ARCH" != amd64 ]; then
    printf '%s\n' "Linux Defragger $VERSION release packages support amd64 only." >&2
    exit 1
fi

case "$BUILD_FLAVOR" in
    generic)
        FLAVOR_DESCRIPTION='Generic x86-64 build for broad amd64 compatibility.'
        ;;
    native)
        FLAVOR_DESCRIPTION='Locally compiled build optimised for this machine CPU.'
        ;;
    *)
        printf 'Unknown LD_BUILD_FLAVOR: %s\n' "$BUILD_FLAVOR" >&2
        exit 1
        ;;
esac

case "$BUILD_TESTING" in
    ON|OFF) ;;
    *)
        printf 'LD_BUILD_TESTING must be ON or OFF, not %s\n' "$BUILD_TESTING" >&2
        exit 1
        ;;
esac

set -- -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLD_ENABLE_WERROR=ON \
    -DBUILD_TESTING="$BUILD_TESTING" \
    -DCMAKE_INSTALL_PREFIX=/usr
if [ "$BUILD_FLAVOR" = generic ]; then
    set -- "$@" -DLD_GENERIC_AMD64=ON -DLD_NATIVE_OPTIMIZATION=OFF
else
    set -- "$@" -DLD_GENERIC_AMD64=OFF -DLD_NATIVE_OPTIMIZATION=ON
fi
cmake "$@"
cmake --build "$BUILD" -j"${BUILD_JOBS:-2}"
DESTDIR="$STAGE/root" cmake --install "$BUILD"

mkdir -p "$STAGE/root/DEBIAN"
INSTALLED_SIZE=$(du -sk "$STAGE/root/usr" | awk '{print $1}')
{
    printf 'Package: linux-defragger\n'
    printf 'Version: %s\n' "$PACKAGE_VERSION"
    printf 'Section: utils\n'
    printf 'Priority: optional\n'
    printf 'Architecture: %s\n' "$ARCH"
    printf 'Maintainer: Shannon Smith\n'
    printf 'X-Linux-Defragger-Build: %s\n' "$BUILD_FLAVOR"
    printf 'Depends: python3, python3-gi, python3-cairo, gir1.2-gtk-3.0, libgtk-3-0t64, policykit-1, udisks2, util-linux, makefs, libext2fs2, libsqlite3-0, libssl3t64\n'
    printf 'Installed-Size: %s\n' "$INSTALLED_SIZE"
    printf 'Description: Safe direct filesystem analysis and canonical layout rewriting\n'
    printf ' Linux Defragger analyses filesystem allocation and safely rewrites\n'
    printf ' supported unmounted FAT, exFAT, NTFS, EXT2/3/4, XFS, Amiga OFS/FFS,\n'
    printf ' Amiga SFS0 and HFS+/HFSX filesystems. Btrfs, classic HFS, APFS, Minix, UFS and ZFS\n'
    printf ' remain analysis-only. The package also includes the separate all-C GTK\n'
    printf ' Linux Defragger Test Media program for building sacrificial field-test disks.\n'
    printf ' %s\n' "$FLAVOR_DESCRIPTION"
} >"$STAGE/root/DEBIAN/control"

(
    cd "$STAGE/root"
    find usr -type f -print0 | sort -z |
        xargs -0 md5sum >DEBIAN/md5sums
)
chmod 0644 "$STAGE/root/DEBIAN/control" "$STAGE/root/DEBIAN/md5sums"
dpkg-deb --root-owner-group --build "$STAGE/root" "$OUTPUT"
printf '%s\n' "$OUTPUT"
