#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

if [ "${LD_INSTALLER_TEST_COMMAND:-0}" = 1 ]; then
    command_name=$(basename -- "$0")
    case "$command_name" in
        dpkg-query)
            case "$*" in
                *'${Version}'*linux-defragger*)
                    [ -f "${LD_INSTALLER_TEST_STATE:?}" ] || exit 1
                    cat "$LD_INSTALLER_TEST_STATE"
                    ;;
                *'${Status}'*)
                    printf '%s\n' 'install ok installed'
                    ;;
                *) exit 1 ;;
            esac
            ;;
        dpkg)
            case "${1:-}" in
                --print-architecture) printf '%s\n' amd64 ;;
                --compare-versions) exec /usr/bin/dpkg "$@" ;;
                --install)
                    /usr/bin/dpkg-deb -f "${2:?package path is required}" Version \
                        >"$LD_INSTALLER_TEST_STATE"
                    ;;
                *) exit 1 ;;
            esac
            ;;
        cmake)
            if [ "${1:-}" = --install ]; then
                : "${DESTDIR:?DESTDIR is required for the fake install}"
                mkdir -p "$DESTDIR/usr/lib/linux-defragger/filesystems/fat"
                : >"$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
                chmod 0755 "$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
            fi
            ;;
        sudo) shift 0; exec "$@" ;;
        gcc|make) exit 0 ;;
        apt-get)
            printf '%s\n' 'The isolated installer test unexpectedly invoked APT.' >&2
            exit 1
            ;;
        *) exit 1 ;;
    esac
    exit 0
fi

[ "$#" -eq 1 ] || {
    printf 'Usage: %s LOCAL-INSTALLER.run\n' "$0" >&2
    exit 2
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
EXPECTED_VERSION="${VERSION}+native1"
RUN=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-installer-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

for command_name in dpkg-query dpkg cmake sudo gcc make apt-get; do
    ln -s "$ROOT/tests/test_local_installer_end_to_end.sh" \
        "$WORK/$command_name"
done

LD_INSTALLER_TEST_COMMAND=1 \
LD_INSTALLER_TEST_STATE="$WORK/installed-version" \
PATH="$WORK:$PATH" \
    "$RUN" >"$WORK/output.log"

[ "$(cat "$WORK/installed-version")" = "$EXPECTED_VERSION" ]
grep -Fq "Linux Defragger $EXPECTED_VERSION is installed." "$WORK/output.log"
grep -Fq 'The native package replaced the generic package' "$WORK/output.log"

printf '%s\n' 'Local compiler end-to-end package-version test passed.'
