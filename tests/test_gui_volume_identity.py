#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for GUI filesystem identity and classic FAT discovery."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path
from subprocess import CompletedProcess

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
if str(GUI) not in sys.path:
    sys.path.insert(0, str(GUI))

from ui.backend_catalog import BackendCatalog
from ui.devices import Volume, discover_volumes
from ui.volume_store import VolumeStore


def _catalog() -> BackendCatalog:
    return BackendCatalog.from_manifest(
        {
            "backends": [
                {"id": "fat12", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "fat16", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "fat32", "aliases": [], "capabilities": 1, "operations": []},
                {"id": "ext4", "aliases": [], "capabilities": 1, "operations": []},
            ]
        }
    )


def _volume(
    *,
    path: str = "/dev/mmcblk0p1",
    fstype: str = "vfat",
    fs_version: str = "FAT12",
    filesystem_uuid: str = "1111-2222",
    partition_uuid: str = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
    image: bool = False,
) -> Volume:
    return Volume(
        catalog=_catalog(),
        path=path,
        name=Path(path).name,
        fstype=fstype,
        label="LD_FAT12",
        size=64 * 1024 * 1024,
        mountpoints=[],
        removable=False,
        readonly=False,
        model="",
        transport="file" if image else "mmc",
        image=image,
        fs_version=fs_version,
        filesystem_uuid=filesystem_uuid,
        partition_uuid=partition_uuid,
    )


def test_lsblk_metadata_refines_generic_vfat() -> None:
    payload = {
        "blockdevices": [
            {
                "name": "mmcblk0p1",
                "path": "/dev/mmcblk0p1",
                "type": "part",
                "fstype": "vfat",
                "fsver": "FAT12",
                "label": "LD_FAT12",
                "uuid": "ABCD-1234",
                "partuuid": "11111111-2222-3333-4444-555555555555",
                "size": 64 * 1024 * 1024,
                "mountpoints": [None],
                "rm": 0,
                "ro": 0,
                "model": "",
                "tran": "mmc",
            }
        ]
    }

    def fake_run(args, **_kwargs):
        assert "FSVER" in args[-1]
        assert "UUID" in args[-1]
        assert "PARTUUID" in args[-1]
        return CompletedProcess(args, 0, stdout=json.dumps(payload), stderr="")

    volumes = discover_volumes(_catalog(), run=fake_run)
    assert len(volumes) == 1
    volume = volumes[0]
    assert volume.normalized_fstype == "fat12"
    assert volume.display_fstype == "fat12"
    assert "— FAT12 —" in volume.display_name
    assert volume.filesystem_uuid == "ABCD-1234"
    assert volume.partition_uuid.startswith("11111111-")


def test_unknown_generic_fat_is_not_labeled_fat32() -> None:
    volume = _volume(fs_version="")
    # The common FAT32 backend remains the compatibility routing fallback, but
    # the GUI must not turn incomplete Linux metadata into a false FAT32 claim.
    assert volume.normalized_fstype == "fat32"
    assert volume.display_fstype == "fat"
    assert "— FAT —" in volume.display_name


def test_rebuilt_same_path_does_not_reuse_cached_map() -> None:
    store = VolumeStore()
    original = _volume()
    assert store.refresh([original]) == 0
    store.remember_map({"filesystem": "FAT12", "marker": "old"})
    assert store.cached_map() is not None

    # Rediscovering the same filesystem instance may safely reuse its map.
    same = _volume()
    assert store.refresh([same], preserve_path=same.path) == 0
    assert store.cached_map() is not None

    # Test Media destroys/recreates both the filesystem UUID and GPT partition
    # UUID while reusing /dev/mmcblk0p1.  The old map must not survive that.
    rebuilt = _volume(
        filesystem_uuid="9999-AAAA",
        partition_uuid="99999999-8888-7777-6666-555555555555",
    )
    assert store.refresh([rebuilt], preserve_path=rebuilt.path) == 0
    assert store.cached_map() is None


def test_uuidless_devices_use_refresh_ephemeral_cache_identity() -> None:
    store = VolumeStore()
    first = _volume(filesystem_uuid="", partition_uuid="")
    store.refresh([first])
    store.remember_map({"filesystem": "FAT12", "marker": "old"})
    assert store.cached_map() is not None

    rediscovered = _volume(filesystem_uuid="", partition_uuid="")
    store.refresh([rediscovered], preserve_path=rediscovered.path)
    assert store.cached_map() is None


def test_replaced_image_at_same_path_does_not_reuse_cached_map() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "same.img"
        path.write_bytes(b"A" * 4096)
        image = _volume(
            path=str(path),
            fstype="fat12",
            fs_version="FAT12",
            filesystem_uuid="",
            partition_uuid="",
            image=True,
        )
        store = VolumeStore()
        store.add_image(image)
        store.remember_map({"filesystem": "FAT12", "marker": "old"})
        assert store.cached_map() is not None

        path.write_bytes(b"B" * 8192)
        assert store.cached_map() is None


def test_invalidate_removes_every_identity_for_one_path() -> None:
    store = VolumeStore()
    first = _volume()
    store.refresh([first])
    store.remember_map({"filesystem": "FAT12"})
    assert store.map_cache
    store.invalidate(first.path)
    assert not store.map_cache


def main() -> None:
    test_lsblk_metadata_refines_generic_vfat()
    test_unknown_generic_fat_is_not_labeled_fat32()
    test_rebuilt_same_path_does_not_reuse_cached_map()
    test_uuidless_devices_use_refresh_ephemeral_cache_identity()
    test_replaced_image_at_same_path_does_not_reuse_cached_map()
    test_invalidate_removes_every_identity_for_one_path()
    print("GUI volume identity and FAT discovery regression tests passed")


if __name__ == "__main__":
    main()
