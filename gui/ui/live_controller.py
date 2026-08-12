# SPDX-License-Identifier: GPL-3.0-or-later
"""Typed, GTK-neutral interpretation of live worker events."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from core.protocol import EngineEventParser, OperationResult

from .formatting import human_bytes
from .live_map import LiveMapUpdater
from .operation_status import operation_display_name


@dataclass(frozen=True, slots=True)
class LiveEventOutcome:
    consumed: bool
    status: str | None = None
    log_messages: tuple[str, ...] = ()
    operation_result: OperationResult | None = None
    map_changed: bool = False
    draw_immediately: bool = False
    fragmented_value: str | None = None
    free_value: str | None = None


class LiveEventController:
    """Apply worker protocol events to a map model and return view updates."""

    def consume(
        self,
        line: str,
        map_data: dict[str, Any] | None,
        *,
        purpose: str,
    ) -> LiveEventOutcome:
        try:
            event = EngineEventParser.parse(line)
        except (ValueError, json.JSONDecodeError) as exc:
            return LiveEventOutcome(
                consumed=True,
                log_messages=(f"Engine event could not be decoded: {exc}",),
            )
        if event is None:
            return LiveEventOutcome(consumed=False)
        if event.kind == "result":
            return LiveEventOutcome(
                consumed=True,
                operation_result=event.operation_result(),
            )
        if event.kind == "phase":
            message = str(event.payload.get("message", "Operation phase changed"))
            return LiveEventOutcome(
                consumed=True,
                status=message,
                log_messages=(message,),
            )
        if not map_data or not isinstance(map_data.get("cells"), list):
            return LiveEventOutcome(consumed=True)

        try:
            updater = LiveMapUpdater(map_data)
            if event.kind == "live-reset":
                updater.reset(event.payload)
                return LiveEventOutcome(
                    consumed=True,
                    status=(
                        "Canonical filesystem layout initialised · live allocation "
                        "writes are now visible"
                    ),
                    map_changed=True,
                    draw_immediately=True,
                )
            if event.kind in {"live-range", "live-ranges"}:
                summary = updater.ranges(
                    event.payload,
                    plural=event.kind == "live-ranges",
                )
                display_name = operation_display_name(purpose or "defrag")
                object_text = (
                    f" · {summary.objects_done:,}/{summary.objects_total:,} objects"
                    if summary.objects_done and summary.objects_total
                    else ""
                )
                return LiveEventOutcome(
                    consumed=True,
                    status=(
                        f"Live allocation update · {display_name} pass "
                        f"{summary.pass_number}{object_text} · "
                        f"{human_bytes(summary.moved_total_bytes)} relocated · "
                        "fragmentation recalculated at completion"
                    ),
                    map_changed=True,
                    draw_immediately=summary.sequence <= 1,
                )
            if event.kind == "live-map":
                updater.cells_delta(event.payload)
                fragmented_value = None
                if (
                    "fragmented_files" in map_data
                    and "fragmented_directories" in map_data
                ):
                    fragmented_value = (
                        f"{map_data['fragmented_files']:,} files · "
                        f"{map_data['fragmented_directories']:,} dirs"
                    )
                free_value = None
                if "free_clusters" in map_data:
                    cluster_size = int(
                        map_data.get("cluster_size")
                        or map_data.get("unit_size")
                        or 0
                    )
                    free_bytes = int(map_data["free_clusters"]) * cluster_size
                    capacity = int(
                        map_data.get("data_clusters")
                        or map_data.get("total_units")
                        or 0
                    ) * cluster_size
                    percent = 100.0 * free_bytes / capacity if capacity else 0.0
                    free_value = f"{human_bytes(free_bytes)} ({percent:.1f}%)"
                return LiveEventOutcome(
                    consumed=True,
                    status="Live allocation map updated",
                    map_changed=True,
                    fragmented_value=fragmented_value,
                    free_value=free_value,
                )
        except Exception as exc:
            return LiveEventOutcome(
                consumed=True,
                log_messages=(
                    f"Live allocation update could not be applied: {exc}",
                ),
            )
        return LiveEventOutcome(consumed=True)
