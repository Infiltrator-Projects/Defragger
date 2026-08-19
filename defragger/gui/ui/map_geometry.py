# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure allocation-grid geometry shared by drawing and tooltip hit testing."""

from __future__ import annotations

from dataclasses import dataclass
from math import ceil, sqrt


@dataclass(frozen=True, slots=True)
class MapGeometry:
    width: int
    height: int
    columns: int
    rows: int
    cell_count: int

    @property
    def uses_one_block_per_cell(self) -> bool:
        return self.cell_count <= self.width * self.height


def allocation_grid(cell_count: int, width: int, height: int) -> MapGeometry:
    """Fit row-major allocation cells into near-square visible blocks."""

    if cell_count <= 0:
        raise ValueError("allocation grid needs at least one cell")
    width = max(1, int(width))
    height = max(1, int(height))
    pixels = width * height
    if cell_count > pixels:
        return MapGeometry(width, height, width, height, cell_count)

    minimum_columns = max(1, ceil(cell_count / height))
    maximum_columns = min(width, cell_count)
    ideal_columns = max(1, round(sqrt(cell_count * width / height)))
    columns = max(minimum_columns, min(maximum_columns, ideal_columns))
    rows = ceil(cell_count / columns)
    return MapGeometry(width, height, columns, rows, cell_count)


def block_bounds(geometry: MapGeometry, index: int) -> tuple[int, int, int, int]:
    """Return the exact non-overlapping pixel bounds for a visible cell block."""

    if not geometry.uses_one_block_per_cell or index < 0 or index >= geometry.cell_count:
        raise ValueError("cell does not have an individual visible block")
    row, column = divmod(index, geometry.columns)
    x0 = column * geometry.width // geometry.columns
    x1 = (column + 1) * geometry.width // geometry.columns
    y0 = row * geometry.height // geometry.rows
    y1 = (row + 1) * geometry.height // geometry.rows
    return x0, y0, x1, y1


def _axis_slot(position: int, pixels: int, slots: int) -> int:
    return min(slots - 1, (((position + 1) * slots) - 1) // pixels)


def source_cell_at(geometry: MapGeometry, x: int, y: int) -> int | None:
    """Map a pointer position to the same source cell used by the renderer."""

    if x < 0 or y < 0 or x >= geometry.width or y >= geometry.height:
        return None
    if geometry.uses_one_block_per_cell:
        column = _axis_slot(int(x), geometry.width, geometry.columns)
        row = _axis_slot(int(y), geometry.height, geometry.rows)
        index = row * geometry.columns + column
        return index if index < geometry.cell_count else None

    pixel_index = int(y) * geometry.width + int(x)
    return min(
        geometry.cell_count - 1,
        pixel_index * geometry.cell_count // (geometry.width * geometry.height),
    )
