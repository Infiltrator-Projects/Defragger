#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Functional tests for the GUI's non-GTK module boundaries."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from backends.base import CAP_DEFRAG, CAP_GROWTH_DEFRAG, CAP_RECOVER
from core.protocol import EngineEventParser, OperationResult
from ui.backend_catalog import BackendCatalog
from ui.devices import Volume
from ui.map_geometry import allocation_grid, block_bounds, source_cell_at
from ui.map_presenter import AllocationMapError, present_allocation_map
from ui.operation_status import successful_completion


def _manifest(backend_id: str = "ext4", alias: str = "ext3") -> dict:
    return {
        "schema": 2,
        "backends": [
            {
                "id": backend_id,
                "aliases": [alias],
                "capabilities": CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER,
                "operations": [
                    {"name": "defrag", "label": "Defragment"},
                    {"name": "growth-defrag", "label": "Growth Defrag"},
                    {"name": "recover", "label": "Recover"},
                ],
            }
        ],
    }


def _cells() -> list[dict[str, int]]:
    return [
        {
            "start": 0,
            "end": 3,
            "free": 1,
            "used": 3,
            "fragmented": 2,
            "directory": 1,
        },
        {
            "start": 4,
            "end": 7,
            "free": 4,
            "used": 0,
            "fragmented": 0,
            "directory": 0,
        },
    ]


def test_catalog_is_validated_immutable_and_instance_owned() -> None:
    first = BackendCatalog.from_manifest(_manifest())
    second = BackendCatalog.from_manifest(_manifest("xfs", "xfs-test"))
    assert first.normalize("EXT3") == "ext4"
    assert first.capabilities_for("ext3") & CAP_DEFRAG
    assert set(first.operations_for("ext4")) == {
        "defrag",
        "growth-defrag",
        "recover",
    }
    try:
        first.aliases["new"] = "ext4"  # type: ignore[index]
    except TypeError:
        pass
    else:
        raise AssertionError("backend catalogue is mutable")

    ext_volume = Volume(
        catalog=first,
        path="/dev/ext",
        name="ext",
        fstype="ext3",
        label="",
        size=1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="",
    )
    xfs_volume = Volume(
        catalog=second,
        path="/dev/xfs",
        name="xfs",
        fstype="xfs-test",
        label="",
        size=1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="",
    )
    assert ext_volume.normalized_fstype == "ext4"
    assert xfs_volume.normalized_fstype == "xfs"

    ambiguous = {
        "backends": [
            {"id": "one", "aliases": ["same"], "operations": []},
            {"id": "two", "aliases": ["same"], "operations": []},
        ]
    }
    try:
        BackendCatalog.from_manifest(ambiguous)
    except ValueError:
        pass
    else:
        raise AssertionError("ambiguous backend alias was accepted")


def test_map_presenter_validates_and_normalises_fat_map() -> None:
    data = {
        "filesystem": "FAT32",
        "volume_id": "1234abcd",
        "cluster_size": 4096,
        "data_clusters": 8,
        "free_clusters": 5,
        "regular_files": 2,
        "directories": 1,
        "fragmented_files": 1,
        "fragmented_directories": 0,
        "free_gaps_below_highest": 2,
        "cell_count": 2,
        "cells": _cells(),
    }
    view = present_allocation_map(data, CAP_DEFRAG)
    assert view.capacity_value == "32.0 KB"
    assert view.free_value == "20.0 KB (62.5%)"
    assert view.fragmentation_value == "1 files · 0 dirs"
    assert view.unit_label == "clusters"
    assert view.caption.startswith("Allocation grid:")
    assert view.cells[0]["outside"] == 0
    assert view.analysis_log.endswith("1 fragmented files, 0 fragmented directories.")


def test_small_filesystems_use_square_allocation_blocks_not_row_stripes() -> None:
    geometry = allocation_grid(2044, 1190, 260)
    assert geometry.uses_one_block_per_cell
    assert geometry.columns == 97
    assert geometry.rows == 22

    first = block_bounds(geometry, 0)
    last_in_first_row = block_bounds(geometry, geometry.columns - 1)
    first_in_second_row = block_bounds(geometry, geometry.columns)
    assert first[2] - first[0] in (12, 13)
    assert first[3] - first[1] in (11, 12)
    assert last_in_first_row[1] == first[1]
    assert first_in_second_row[1] > first[1]
    assert source_cell_at(geometry, first[0], first[1]) == 0
    assert source_cell_at(
        geometry,
        first_in_second_row[0],
        first_in_second_row[1],
    ) == geometry.columns

    sampled = allocation_grid(300, 10, 10)
    assert not sampled.uses_one_block_per_cell
    assert source_cell_at(sampled, 9, 9) == 297


def test_map_presenter_handles_domain_and_swap_maps() -> None:
    domain = {
        "backend": "read-only-domain",
        "filesystem": "ext4",
        "unit_size": 4096,
        "total_units": 8,
        "cell_count": 2,
        "total_bytes": 8 * 4096,
        "filesystem_bytes": 6 * 4096,
        "outside_bytes": 2 * 4096,
        "free_bytes": 3 * 4096,
        "used_bytes": 3 * 4096,
        "unknown_bytes": 0,
        "cells": _cells(),
    }
    writable = present_allocation_map(
        domain,
        CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER,
    )
    assert writable.fragmentation_value == "Not calculated"
    assert "white tail" in writable.caption
    assert "available: Defragment, Growth Defrag, Recover" in writable.status

    read_only = present_allocation_map(domain, 0)
    assert read_only.fragmentation_value == "Not available"
    assert "read-only allocation map" in read_only.status

    swap = dict(domain)
    swap.update(
        filesystem="swap",
        filesystem_bytes=8 * 4096,
        outside_bytes=0,
        free_bytes=6 * 4096,
        used_bytes=2 * 4096,
        details={"active": True, "page_size": 4096},
    )
    swap_view = present_allocation_map(swap, 0)
    assert swap_view.files_title == "Usage"
    assert swap_view.files_value == "8.0 KB used · 2 pages"
    assert swap_view.fragmentation_value == "Not applicable"
    assert "aggregate usage only" in swap_view.caption
    assert "2 pages" in swap_view.status
    assert "physical occupied slot locations are unavailable" in swap_view.analysis_log

    swap["used_bytes"] = 4096
    swap["free_bytes"] = 7 * 4096
    one_page = present_allocation_map(swap, 0)
    assert one_page.files_value == "4.0 KB used · 1 page"


def test_invalid_map_is_rejected_before_widget_state_changes() -> None:
    invalid = {
        "filesystem": "fat32",
        "cluster_size": 4096,
        "data_clusters": 8,
        "free_clusters": 1,
        "regular_files": 1,
        "directories": 1,
        "fragmented_files": 0,
        "fragmented_directories": 0,
        "free_gaps_below_highest": 0,
        "cell_count": 3,
        "cells": _cells(),
    }
    try:
        present_allocation_map(invalid, 0)
    except AllocationMapError:
        pass
    else:
        raise AssertionError("map with a false cell count was accepted")


def test_result_protocol_replaces_worker_output_text_matching() -> None:
    event = EngineEventParser.parse(
        '@@RESULT {"operation":"growth-defrag","status":"not-needed","message":""}'
    )
    assert event is not None
    result = event.operation_result()
    presentation = successful_completion("growth-defrag", result)
    assert presentation.progress_text == "Not needed"
    assert presentation.post_analysis_status is not None

    ordinary = successful_completion(
        "growth-defrag",
        OperationResult("growth-defrag", "completed"),
    )
    assert ordinary.progress_text == "Complete"

    failed_event = EngineEventParser.parse(
        '@@RESULT {"operation":"defrag","status":"failed","message":"unsupported layout"}'
    )
    assert failed_event is not None
    assert failed_event.operation_result().status == "failed"

    try:
        EngineEventParser.parse(
            '@@RESULT {"operation":"growth-defrag","status":"maybe"}'
        )
    except ValueError:
        pass
    else:
        raise AssertionError("invalid worker result status was accepted")

    controller_source = (GUI / "ui" / "command_runner.py").read_text()
    assert "Growth Defrag status:          Not needed;" not in controller_source


def test_about_dialog_matches_the_standard_project_identity() -> None:
    source = (GUI / "ui" / "window_view.py").read_text()
    for required in (
        'APP_ICON_NAME = "io.github.linuxdefragger"',
        'COPYRIGHT = "Copyright © 2026 Shannon Smith"',
        'PROJECT_URL = "https://github.com/The-First-Infiltrator/Defragger"',
        "dialog.set_logo_icon_name(APP_ICON_NAME)",
        "dialog.set_comments(ABOUT_COMMENTS)",
        "Shannon Smith — Author and project maintainer",
        'dialog.set_website_label("Website")',
        "dialog.set_copyright(COPYRIGHT)",
        "dialog.set_license(ABOUT_LICENSE)",
        "dialog.set_wrap_license(True)",
    ):
        assert required in source
    assert "Operation engine:" not in source
    assert "hfsutils" not in source


def main() -> None:
    test_catalog_is_validated_immutable_and_instance_owned()
    test_map_presenter_validates_and_normalises_fat_map()
    test_small_filesystems_use_square_allocation_blocks_not_row_stripes()
    test_map_presenter_handles_domain_and_swap_maps()
    test_invalid_map_is_rejected_before_widget_state_changes()
    test_result_protocol_replaces_worker_output_text_matching()
    test_about_dialog_matches_the_standard_project_identity()
    print("GUI model and worker-result contract tests passed")


if __name__ == "__main__":
    main()
