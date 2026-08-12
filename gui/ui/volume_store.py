# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-window volume selection and allocation-map cache.

The GTK window should not own collection policy.  ``VolumeStore`` keeps device
and image volumes, selection, and cached analysis together without importing
GTK or launching external programs.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable

from .devices import Volume


@dataclass(slots=True)
class VolumeStore:
    """Mutable storage state owned by exactly one application window."""

    volumes: list[Volume] = field(default_factory=list)
    current: Volume | None = None
    map_cache: dict[str, dict[str, Any]] = field(default_factory=dict)

    def refresh(
        self,
        discovered: Iterable[Volume],
        *,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> int:
        """Replace physical volumes, retain opened images, and return selection."""

        if clear_cache:
            self.map_cache.clear()
        images = [volume for volume in self.volumes if volume.image]
        image_paths = {volume.path for volume in images}
        physical = [volume for volume in discovered if volume.path not in image_paths]
        self.volumes = physical + images

        selected_path = preserve_path or (self.current.path if self.current else None)
        selected_index = next(
            (
                index
                for index, volume in enumerate(self.volumes)
                if volume.path == selected_path
            ),
            -1,
        )
        if selected_index < 0 and self.volumes:
            selected_index = 0
        self.select(selected_index)
        return selected_index

    def add_image(self, volume: Volume) -> int:
        """Add or replace an image volume and select it."""

        self.volumes = [item for item in self.volumes if item.path != volume.path]
        self.volumes.append(volume)
        return self.select(len(self.volumes) - 1)

    def select(self, index: int) -> int:
        """Select an index, or clear selection when it is outside the list."""

        if 0 <= index < len(self.volumes):
            self.current = self.volumes[index]
            return index
        self.current = None
        return -1

    def cached_map(self) -> dict[str, Any] | None:
        return self.map_cache.get(self.current.path) if self.current else None

    def remember_map(self, data: dict[str, Any]) -> None:
        if self.current is not None:
            self.map_cache[self.current.path] = data

    def invalidate(self, path: str) -> None:
        self.map_cache.pop(path, None)
