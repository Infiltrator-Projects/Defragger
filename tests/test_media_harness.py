#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "linux-defragger-media-harness.py"
spec = importlib.util.spec_from_file_location("ld_media_harness", SCRIPT)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def expect_runtime_error(fn, needle: str) -> None:
    try:
        fn()
    except RuntimeError as exc:
        assert needle in str(exc), (needle, str(exc))
    else:
        raise AssertionError(f"expected RuntimeError containing {needle!r}")


def test_layout() -> None:
    labels = [item.partlabel for item in module.SPECS]
    keys = [item.key for item in module.SPECS]
    assert len(labels) == len(set(labels)) == 18
    assert len(keys) == len(set(keys)) == 18
    text = module.sfdisk_script()
    assert text.startswith("label: gpt\n")
    assert text.count('name="LD_') == 18
    assert 'name="LD_SWAP"' in text and "type=swap" in text
    allocated = sum(item.size_mib for item in module.SPECS) * module.MIB
    assert module.required_capacity_bytes() > allocated
    assert module.required_capacity_bytes() < 40 * module.GIB


def test_payload_profiles() -> None:
    by_key = {item.key: item for item in module.SPECS}
    assert module.target_payload_bytes(by_key["fat12"]) == 4 * module.MIB
    for key in (
        "fat16", "fat32", "exfat", "ntfs", "ext2", "ext3", "ext4", "xfs",
        "btrfs", "affs", "hfs", "hfsplus", "minix", "ufs", "zfs", "apfs",
    ):
        assert module.target_payload_bytes(by_key[key]) == 200 * module.MIB, key
    assert module.target_payload_bytes(by_key["swap"]) == 0


def test_safety() -> None:
    device = "/dev/test-removable"
    good = {
        "type": "disk",
        "size": module.required_capacity_bytes() + module.GIB,
        "rm": True,
        "ro": False,
        "tran": "usb",
    }
    module.validate_device_safety(
        device, good, allow_non_removable=False, protected_disks=set()
    )
    expect_runtime_error(
        lambda: module.validate_device_safety(
            device, dict(good, type="part"), allow_non_removable=False,
            protected_disks=set(),
        ),
        "whole-disk",
    )
    expect_runtime_error(
        lambda: module.validate_device_safety(
            device, good, allow_non_removable=False, protected_disks={device}
        ),
        "system/boot disk",
    )
    expect_runtime_error(
        lambda: module.validate_device_safety(
            device, dict(good, ro=True), allow_non_removable=False,
            protected_disks=set(),
        ),
        "read-only",
    )
    expect_runtime_error(
        lambda: module.validate_device_safety(
            device, dict(good, size=8 * module.GIB), allow_non_removable=False,
            protected_disks=set(),
        ),
        "too small",
    )
    internal = dict(good, rm=False, tran="nvme")
    expect_runtime_error(
        lambda: module.validate_device_safety(
            device, internal, allow_non_removable=False, protected_disks=set()
        ),
        "non-removable",
    )
    module.validate_device_safety(
        device, internal, allow_non_removable=True, protected_disks=set()
    )


def test_confirmation() -> None:
    assert module.confirmation_token("/dev/example") == "DESTROY /dev/example"


def test_manifest_verification() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        mount = Path(temporary)
        root = mount / module.DATA_DIR
        target = root / "fragmented-files"
        directory = root / "fragmented-directory"
        target.mkdir(parents=True)
        directory.mkdir()
        payload = b"abc123" * 100
        path = target / "fragmented-00.bin"
        path.write_bytes(payload)
        (directory / "one.txt").write_text("one\n")
        manifest = {
            "target_files": [{
                "path": "fragmented-files/fragmented-00.bin",
                "size": len(payload),
                "sha256": module.hashlib.sha256(payload).hexdigest(),
            }],
            "directory_entries": 1,
        }
        assert module.verify_manifest(mount, manifest) == []
        path.write_bytes(b"corrupt")
        errors = module.verify_manifest(mount, manifest)
        assert any(
            "size mismatch" in error or "SHA-256 mismatch" in error
            for error in errors
        )


def test_state_roundtrip() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "state.json"
        expected = {"schema": 1, "filesystems": [{"key": "fat32"}]}
        module.save_state(path, expected)
        assert module.load_state(path) == expected
        json.loads(path.read_text())


def main() -> None:
    test_layout()
    test_payload_profiles()
    test_safety()
    test_confirmation()
    test_manifest_verification()
    test_state_roundtrip()
    print("physical-media harness safety and manifest tests passed")


if __name__ == "__main__":
    main()
