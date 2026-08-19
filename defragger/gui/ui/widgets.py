# SPDX-License-Identifier: GPL-3.0-or-later
"""Reusable GTK widgets for the main window."""

from __future__ import annotations

from typing import Any

from gi.repository import Gtk

from .map_geometry import MapGeometry, allocation_grid, block_bounds, source_cell_at

MIN_MAP_CELLS = 256
MAX_MAP_CELLS = 1048576


class DiskMap(Gtk.DrawingArea):
    """Render allocation data as a dense dynamically sized block grid."""

    COLORS = {
        "free": (0.92, 0.94, 0.96),
        "outside": (0.98, 0.98, 0.99),
        "used": (0.13, 0.43, 0.76),
        "fragmented": (0.94, 0.28, 0.22),
        "directory": (0.48, 0.28, 0.72),
        "unknown": (0.38, 0.40, 0.44),
        "bad": (0.08, 0.08, 0.10),
        "grid": (0.74, 0.77, 0.81),
        "background": (0.98, 0.98, 0.99),
    }

    def __init__(self) -> None:
        super().__init__()
        self.cells: list[dict[str, int]] = []
        self.unit_label = "clusters"
        self._layout: MapGeometry | None = None
        self.set_size_request(640, 260)
        self.set_has_tooltip(True)
        self.connect("draw", self._draw)
        self.connect("query-tooltip", self._query_tooltip)

    def set_cells(self, cells: list[dict[str, int]]) -> None:
        self.cells = cells
        self.queue_draw()

    def set_unit_label(self, label: str) -> None:
        self.unit_label = label

    def desired_cell_count(self, width: int | None = None, height: int | None = None) -> int:
        if width is None or height is None:
            allocation = self.get_allocation()
            width, height = allocation.width, allocation.height
        drawable_pixels = max(1, int(width)) * max(1, int(height))
        return max(MIN_MAP_CELLS, min(MAX_MAP_CELLS, drawable_pixels))

    @staticmethod
    def _mix(a: tuple[float, float, float], b: tuple[float, float, float], ratio: float):
        ratio = max(0.0, min(1.0, ratio))
        return tuple(a[index] * (1.0 - ratio) + b[index] * ratio for index in range(3))

    def _cell_colour(self, cell: dict[str, int]) -> tuple[float, float, float]:
        free = int(cell.get("free", 0))
        outside = int(cell.get("outside", 0))
        used = int(cell.get("used", 0))
        free_like = free + outside
        known_total = max(1, free_like + used)
        free_colour = self._mix(
            self.COLORS["free"],
            self.COLORS["outside"],
            outside / max(1, free_like),
        )
        colour = self._mix(free_colour, self.COLORS["used"], used / known_total)
        overlay_minimum = {"directory": 0.58, "fragmented": 0.62, "bad": 0.52}
        for name in ("directory", "fragmented", "bad"):
            amount = int(cell.get(name, 0))
            if amount:
                ratio = min(1.0, (amount / known_total) ** 0.5)
                # A directory or metadata extent can occupy only one unit inside
                # a many-unit display cell.  Keep it visibly identifiable rather
                # than blending it into ordinary blue allocation.
                ratio = max(overlay_minimum[name], ratio)
                colour = self._mix(colour, self.COLORS[name], ratio)
        unknown = int(cell.get("unknown", 0))
        total = free + outside + used + unknown
        if unknown and total:
            colour = self._mix(colour, self.COLORS["unknown"], unknown / total)
        return colour

    def _draw(self, widget: Gtk.Widget, cr: Any) -> bool:
        allocation = widget.get_allocation()
        width, height = max(1, allocation.width), max(1, allocation.height)
        cr.set_source_rgb(*self.COLORS["background"])
        cr.rectangle(0, 0, width, height)
        cr.fill()
        if not self.cells:
            cr.set_source_rgb(0.38, 0.40, 0.44)
            cr.select_font_face("Sans", 0, 0)
            cr.set_font_size(15)
            message = "Select a supported volume and click Analyse"
            extents = cr.text_extents(message)
            cr.move_to(
                (width - extents.width) / 2 - extents.x_bearing,
                (height - extents.height) / 2 - extents.y_bearing,
            )
            cr.show_text(message)
            return False
        cell_count = len(self.cells)
        self._layout = allocation_grid(cell_count, width, height)
        if self._layout.uses_one_block_per_cell:
            for source_index, cell in enumerate(self.cells):
                x0, y0, x1, y1 = block_bounds(self._layout, source_index)
                cr.set_source_rgb(*self._cell_colour(cell))
                cr.rectangle(float(x0), float(y0), float(x1 - x0), float(y1 - y0))
                cr.fill()
        else:
            total_pixels = width * height
            last_source = -1
            colour = self.COLORS["background"]
            for pixel_index in range(total_pixels):
                source_index = min(
                    cell_count - 1,
                    (pixel_index * cell_count) // total_pixels,
                )
                if source_index != last_source:
                    colour = self._cell_colour(self.cells[source_index])
                    last_source = source_index
                row, column = divmod(pixel_index, width)
                cr.set_source_rgb(*colour)
                cr.rectangle(float(column), float(row), 1.0, 1.0)
                cr.fill()
        return False

    def _query_tooltip(
        self,
        _widget: Gtk.Widget,
        x: int,
        y: int,
        _keyboard_mode: bool,
        tooltip: Gtk.Tooltip,
    ) -> bool:
        if not self.cells or self._layout is None:
            return False
        index = source_cell_at(self._layout, x, y)
        if index is None:
            return False
        cell = self.cells[index]
        tooltip.set_text(
            f"{self.unit_label.capitalize()} {cell['start']:,}–{cell['end']:,}\n"
            f"Used {cell['used']:,} · Free {cell['free']:,} · "
            f"Outside filesystem {cell.get('outside', 0):,} · Unknown {cell.get('unknown', 0):,}\n"
            f"Fragmented {cell['fragmented']:,} · Directory {cell['directory']:,} · "
            f"Metadata/reserved {cell.get('bad', 0):,}"
        )
        return True


class SummaryCard(Gtk.Frame):
    def __init__(self, title: str) -> None:
        super().__init__()
        self.set_shadow_type(Gtk.ShadowType.IN)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        box.set_border_width(10)
        self.title = Gtk.Label(label=title)
        self.title.set_xalign(0)
        self.title.get_style_context().add_class("summary-title")
        self.value = Gtk.Label(label="—")
        self.value.set_xalign(0)
        self.value.get_style_context().add_class("summary-value")
        box.pack_start(self.title, False, False, 0)
        box.pack_start(self.value, False, False, 0)
        self.add(box)

    def set_title(self, title: str) -> None:
        self.title.set_text(title)

    def set_value(self, value: str) -> None:
        self.value.set_text(value)
