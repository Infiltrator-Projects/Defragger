# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK-neutral volume discovery, image opening, selection and map caching."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .backend_catalog import BackendCatalog
from .devices import Volume, discover_volumes
from .engine_client import detect_image_fstype
from .support import safe_journal_name
from .volume_store import VolumeStore


DiscoverVolumes = Callable[[BackendCatalog], list[Volume]]
DetectImageFilesystem = Callable[[str, BackendCatalog], str]


@dataclass(frozen=True, slots=True)
class VolumeSelection:
    """Selection result consumed by the window presentation layer."""

    volume: Volume | None
    cached_map: dict[str, Any] | None

    @property
    def needs_analysis(self) -> bool:
        return self.volume is not None and self.cached_map is None


class VolumeCoordinator:
    """Own the complete per-window volume lifecycle without importing GTK."""

    def __init__(
        self,
        catalog: BackendCatalog,
        *,
        discover: DiscoverVolumes = discover_volumes,
        detect_image: DetectImageFilesystem = detect_image_fstype,
    ) -> None:
        self.catalog = catalog
        self.store = VolumeStore()
        self._discover = discover
        self._detect_image = detect_image

    @property
    def current(self) -> Volume | None:
        return self.store.current

    @property
    def volumes(self) -> tuple[Volume, ...]:
        return tuple(self.store.volumes)

    def refresh(
        self,
        *,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> int:
        """Refresh physical devices while retaining opened image files."""

        return self.store.refresh(
            self._discover(self.catalog),
            preserve_path=preserve_path,
            clear_cache=clear_cache,
        )

    def select(self, index: int) -> VolumeSelection:
        """Select a volume, revalidate its identity, and return a safe cache hit."""

        self.store.select(index)
        selected = self.current
        if selected is not None and not selected.image:
            try:
                fresh = next(
                    (
                        volume
                        for volume in self._discover(self.catalog)
                        if volume.path == selected.path
                    ),
                    None,
                )
            except Exception:
                # Discovery failure must never make an old map look current.
                self.store.invalidate(selected.path)
            else:
                if fresh is None:
                    self.store.invalidate(selected.path)
                else:
                    self.store.replace(fresh)
        return VolumeSelection(self.current, self.store.cached_map())

    def open_image(self, filename: str) -> Volume:
        """Validate, model and select a filesystem image."""

        path = str(Path(filename).resolve())
        fstype = self._detect_image(path, self.catalog)
        size = Path(path).stat().st_size
        volume = Volume(
            catalog=self.catalog,
            path=path,
            name=Path(path).name,
            fstype=fstype,
            label=Path(path).name,
            size=size,
            mountpoints=[],
            removable=False,
            readonly=not os.access(path, os.W_OK),
            model="filesystem image",
            transport="file",
            image=True,
        )
        self.store.add_image(volume)
        return volume

    def remember_map(self, data: dict[str, Any]) -> None:
        self.store.remember_map(data)

    def cached_map(self) -> dict[str, Any] | None:
        return self.store.cached_map()

    def invalidate(self, path: str) -> None:
        self.store.invalidate(path)

    def journal_path(self, state_directory: Path) -> str:
        volume = self.current
        if volume is None:
            return ""
        return str(
            state_directory / f"{safe_journal_name(volume.path)}.journal"
        )
