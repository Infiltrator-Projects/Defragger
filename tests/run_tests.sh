#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAT_WORKER=${1:-"$ROOT/build/linux-defragger-fat-worker"}
BUILD_DIR=$(CDPATH= cd -- "$(dirname -- "$FAT_WORKER")" && pwd)
HFS_ANALYSER="$BUILD_DIR/hfs_analyser"
XFS_WORKER="$BUILD_DIR/linux-defragger-xfs-worker"
XFS_NATIVE_TEST="$BUILD_DIR/linux-defragger-xfs-native-test"
XFS_METADATA_TEST="$BUILD_DIR/linux-defragger-xfs-metadata-test"
EXPECTED_VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="$ROOT/gui"
export LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR"

fail() {
    echo "TEST FAILURE: $*" >&2
    exit 1
}

[[ -x "$FAT_WORKER" ]] || fail "FAT worker is not executable: $FAT_WORKER"
[[ -x "$HFS_ANALYSER" ]] || fail "HFS analyser is not executable: $HFS_ANALYSER"
[[ -x "$XFS_WORKER" ]] || fail "XFS worker is not executable: $XFS_WORKER"
export LINUX_DEFRAGGER_XFS_WORKER="$XFS_WORKER"

version=$($FAT_WORKER --version)
[[ "$version" == "linux-defragger-fat-worker $EXPECTED_VERSION" ]] || \
    fail "unexpected FAT worker version: $version"

python3 -m compileall -q "$ROOT/gui"
"$ROOT/tests/test_release_artifacts.sh"
"$ROOT/tests/run_typecheck.sh"
python3 "$ROOT/tests/test_spdx_licensing.py"
python3 "$ROOT/tests/test_no_external_fs_tools.py"
python3 "$ROOT/tests/test_architecture.py"
python3 "$ROOT/tests/test_analysis_plugins.py"
python3 "$ROOT/tests/test_ext_catalog_fuzz.py"
PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_planner_properties.py"
python3 "$ROOT/tests/test_gui_analysis_start.py"
python3 "$ROOT/tests/test_gui_models.py"
python3 "$ROOT/tests/test_gui_services.py"
python3 "$ROOT/tests/test_transactions.py"
python3 "$ROOT/tests/test_range_helpers.py"
python3 "$ROOT/tests/test_safety.py"
python3 "$ROOT/tests/test_media_harness.py"
python3 "$ROOT/tests/test_worker_entrypoints.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_native_top3.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_affs_native.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_hfsplus_native.py"
LINUX_DEFRAGGER_BUILD_DIR="$BUILD_DIR" PYTHONPATH="$ROOT/gui:$ROOT/tests" python3 "$ROOT/tests/test_hfs_native.py"
python3 "$ROOT/tests/test_xfs_writer.py"
if [[ -x "$XFS_NATIVE_TEST" ]]; then "$XFS_NATIVE_TEST"; fi
if [[ -x "$XFS_METADATA_TEST" ]]; then "$XFS_METADATA_TEST"; fi

# FAT32: a fragmented file must become contiguous and occupy the first legal
# data clusters directly after the root directory.
python3 "$ROOT/tests/make_fragmented_image.py" "$WORK/fragmented.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/fragmented.img" \
    --write --confirm "$WORK/fragmented.img" --journal "$WORK/fragmented.journal" \
    --live-map-cells 1024 >"$WORK/fragmented.log" 2>&1
python3 "$ROOT/tests/verify_defragged_image.py" "$WORK/fragmented.img"
grep -q '@@LIVE_MAP ' "$WORK/fragmented.log" || fail "Defragment emitted no live map records"
grep -q 'no free clusters below the final allocation' "$WORK/fragmented.log" || \
    fail "Defragment did not verify the zero-gap invariant"

# FAT32: fragmented root/subdirectory/file chains must all be rebuilt into one
# packed allocation prefix, with directory references corrected.
python3 "$ROOT/tests/make_fragmented_directory_image.py" "$WORK/directories.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/directories.img" \
    --write --confirm "$WORK/directories.img" --journal "$WORK/directories.journal" \
    >"$WORK/directories.log" 2>&1
python3 "$ROOT/tests/verify_directory_defrag.py" "$WORK/directories.img"

# FAT32: even zero-fragment input must be rewritten when physical holes remain.
python3 "$ROOT/tests/make_gapped_contiguous_image.py" "$WORK/gapped.img" >/dev/null
"$FAT_WORKER" defrag "$WORK/gapped.img" \
    --write --confirm "$WORK/gapped.img" --journal "$WORK/gapped.journal" \
    >"$WORK/gapped.log" 2>&1
python3 "$ROOT/tests/verify_gapped_contiguous.py" "$WORK/gapped.img"
"$FAT_WORKER" defrag "$WORK/gapped.img" \
    --write --confirm "$WORK/gapped.img" --journal "$WORK/gapped-second.journal" \
    >"$WORK/gapped-second.log" 2>&1
grep -q 'Not needed; canonical packed layout verified' "$WORK/gapped-second.log" || \
    fail "second Defragment pass was not idempotent"
grep -q '@@RESULT {"operation":"defrag","status":"not-needed"' "$WORK/gapped-second.log" || \
    fail "FAT did not emit a typed not-needed result"

# FAT32 Growth Defrag: exact ten-percent gaps, contiguous chains and payloads.
python3 "$ROOT/tests/make_growth_defrag_image.py" "$WORK/growth.img" >/dev/null
"$FAT_WORKER" growth-defrag "$WORK/growth.img" \
    --write --confirm "$WORK/growth.img" --journal "$WORK/growth.journal" \
    --growth-percent 10 --live-map-cells 1024 >"$WORK/growth.log" 2>&1
python3 "$ROOT/tests/verify_growth_defrag.py" "$WORK/growth.img"
grep -q '@@LIVE_MAP ' "$WORK/growth.log" || fail "Growth Defrag emitted no live map records"
grep -q '@@RESULT {"operation":"growth-defrag","status":"completed"' "$WORK/growth.log" || \
    fail "FAT did not emit a typed completion result"
if "$FAT_WORKER" growth-defrag "$WORK/growth.img" \
    --write --confirm "$WORK/growth.img" --journal "$WORK/bad-growth.journal" \
    --growth-percent 5 >"$WORK/bad-growth.log" 2>&1; then
    fail "Growth Defrag accepted a reserve other than ten percent"
fi

# FAT12 and FAT16 use the same raw canonical planner.
for kind in fat12 fat16; do
    python3 "$ROOT/tests/make_fat12_16_image.py" "$kind" "$WORK/$kind-defrag.img" fragmented >/dev/null
    "$FAT_WORKER" defrag "$WORK/$kind-defrag.img" \
        --write --confirm "$WORK/$kind-defrag.img" --journal "$WORK/$kind-defrag.journal" \
        >"$WORK/$kind-defrag.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" "$kind" "$WORK/$kind-defrag.img"

    python3 "$ROOT/tests/make_fat12_16_image.py" "$kind" "$WORK/$kind-growth.img" fragmented >/dev/null
    "$FAT_WORKER" growth-defrag "$WORK/$kind-growth.img" \
        --write --confirm "$WORK/$kind-growth.img" --journal "$WORK/$kind-growth.journal" \
        --growth-percent 10 >"$WORK/$kind-growth.log" 2>&1
    python3 "$ROOT/tests/verify_growth_fat12_16.py" "$kind" "$WORK/$kind-growth.img"
done

# Write confirmation is mandatory and the classic HFS helper is read-only.
if "$FAT_WORKER" defrag "$WORK/gapped.img" --write --confirm wrong \
    --journal "$WORK/wrong-confirm.journal" >"$WORK/wrong-confirm.log" 2>&1; then
    fail "engine accepted an incorrect write confirmation"
fi
if "$HFS_ANALYSER" defrag /dev/null >"$WORK/hfs-write.log" 2>&1; then
    fail "read-only HFS analyser accepted a write operation"
fi

printf 'Linux Defragger %s focused tests passed.\n' "$EXPECTED_VERSION"
