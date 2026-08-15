#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Prepare and verify destructive multi-filesystem field-test media.

"""Build a deliberately fragmented Linux Defragger field-test card.

This is test tooling, not part of any production filesystem engine. It is
intentionally conservative: the target whole-disk block device is always
explicit, system disks are rejected, non-removable disks require an override,
and destructive preparation requires typing an exact per-device token.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

MIB = 1024 * 1024
GIB = 1024 * MIB
STATE_ROOT = Path("/var/tmp/linux-defragger-media-harness")
DATA_DIR = "LinuxDefragger-TestData"
DEFAULT_CELLS = 4096


@dataclass(frozen=True)
class FilesystemSpec:
    key: str
    partlabel: str
    size_mib: int
    payload_mib: int
    creator: str
    package_hint: str = ""
    note: str = ""


SPECS: tuple[FilesystemSpec, ...] = (
    FilesystemSpec("fat12", "LD_FAT12", 64, 4, "fat12", "dosfstools",
                   "FAT12 uses a small payload because a 200 MiB payload is not an appropriate FAT12 field test."),
    FilesystemSpec("fat16", "LD_FAT16", 512, 200, "fat16", "dosfstools"),
    FilesystemSpec("fat32", "LD_FAT32", 2048, 200, "fat32", "dosfstools"),
    FilesystemSpec("exfat", "LD_EXFAT", 2048, 200, "exfat", "exfatprogs"),
    FilesystemSpec("ntfs", "LD_NTFS", 2048, 200, "ntfs", "ntfs-3g"),
    FilesystemSpec("ext2", "LD_EXT2", 2048, 200, "ext2"),
    FilesystemSpec("ext3", "LD_EXT3", 2048, 200, "ext3"),
    FilesystemSpec("ext4", "LD_EXT4", 2048, 200, "ext4"),
    FilesystemSpec("xfs", "LD_XFS", 2048, 200, "xfs", "xfsprogs"),
    FilesystemSpec("btrfs", "LD_BTRFS", 2048, 200, "btrfs", "btrfs-progs"),
    FilesystemSpec("affs", "LD_AFFS", 1024, 200, "affs", "",
                   "Created only when an mkfs.affs helper is installed; otherwise the partition is left as a reserved test slot."),
    FilesystemSpec("hfs", "LD_HFS", 1024, 200, "hfs", "hfsutils"),
    FilesystemSpec("hfsplus", "LD_HFSPLUS", 2048, 200, "hfsplus", "hfsprogs"),
    FilesystemSpec("minix", "LD_MINIX", 1024, 200, "minix"),
    FilesystemSpec("ufs", "LD_UFS", 2048, 200, "ufs", "",
                   "Created only when an mkfs.ufs helper is installed; Linux may still be unable to mount it read/write for population."),
    FilesystemSpec("zfs", "LD_ZFS", 4096, 200, "zfs", "zfsutils-linux",
                   "Uses an isolated single-partition pool and exports it after population."),
    FilesystemSpec("apfs", "LD_APFS", 4096, 200, "manual", "",
                   "No Linux APFS creator is assumed. The real partition slot is created but never populated with a fake filesystem."),
    FilesystemSpec("swap", "LD_SWAP", 1024, 0, "swap", "",
                   "Swap contains no files; use swapon/swapoff for active/inactive analysis tests."),
)


def required_capacity_bytes() -> int:
    return sum(spec.size_mib for spec in SPECS) * MIB + GIB


def confirmation_token(device: str) -> str:
    return f"DESTROY {os.path.realpath(device)}"


def sfdisk_script() -> str:
    lines = ["label: gpt", ""]
    for spec in SPECS:
        ptype = "swap" if spec.key == "swap" else "linux"
        lines.append(f'size={spec.size_mib}MiB, type={ptype}, name="{spec.partlabel}"')
    return "\n".join(lines) + "\n"


def testdata_arguments(spec: FilesystemSpec) -> list[str]:
    if spec.payload_mib <= 0:
        return []
    if spec.key == "fat12":
        return ["--anchors", "8", "--anchor-mb", "1", "--files", "2",
                "--chunks", "16", "--chunk-kb", "128", "--force"]
    # 8 * 100 * 256 KiB = 200 MiB of deterministic target-file payload.
    return ["--anchors", "64", "--anchor-mb", "2", "--files", "8",
            "--chunks", "100", "--chunk-kb", "256", "--force"]


def target_payload_bytes(spec: FilesystemSpec) -> int:
    args = testdata_arguments(spec)
    if not args:
        return 0
    values = {args[index]: args[index + 1] for index in range(0, len(args) - 1, 2)
              if args[index].startswith("--") and args[index] != "--force"}
    return int(values["--files"]) * int(values["--chunks"]) * int(values["--chunk-kb"]) * 1024


def run(command: Sequence[str], *, input_text: str | None = None,
        capture: bool = False, check: bool = True,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    print(f"+ {shlex.join([str(item) for item in command])}", file=sys.stderr, flush=True)
    result = subprocess.run(
        [str(item) for item in command], input=input_text, text=True,
        capture_output=capture, check=False, env=env,
    )
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(f"command failed ({result.returncode}): {shlex.join(command)}{suffix}")
    return result


def which_any(*names: str) -> str | None:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def creator_command(spec: FilesystemSpec, partition: str) -> list[str] | None:
    creator = spec.creator
    if creator == "fat12":
        tool = which_any("mkfs.fat", "mkfs.vfat")
        return [tool, "-F", "12", "-n", spec.partlabel, partition] if tool else None
    if creator == "fat16":
        tool = which_any("mkfs.fat", "mkfs.vfat")
        return [tool, "-F", "16", "-n", spec.partlabel, partition] if tool else None
    if creator == "fat32":
        tool = which_any("mkfs.fat", "mkfs.vfat")
        return [tool, "-F", "32", "-n", spec.partlabel, partition] if tool else None
    if creator == "exfat":
        tool = which_any("mkfs.exfat")
        return [tool, "-L", spec.partlabel, partition] if tool else None
    if creator == "ntfs":
        tool = which_any("mkntfs", "mkfs.ntfs")
        return [tool, "-F", "-L", spec.partlabel, partition] if tool else None
    if creator in {"ext2", "ext3", "ext4"}:
        tool = which_any(f"mkfs.{creator}")
        return [tool, "-F", "-L", spec.partlabel, partition] if tool else None
    if creator == "xfs":
        tool = which_any("mkfs.xfs")
        return [tool, "-f", "-L", spec.partlabel, partition] if tool else None
    if creator == "btrfs":
        tool = which_any("mkfs.btrfs")
        return [tool, "-f", "-L", spec.partlabel, partition] if tool else None
    if creator == "affs":
        tool = which_any("mkfs.affs")
        return [tool, partition] if tool else None
    if creator == "hfs":
        tool = which_any("hformat")
        return [tool, "-l", spec.partlabel, partition] if tool else None
    if creator == "hfsplus":
        tool = which_any("mkfs.hfsplus", "mkfs.hfs")
        return [tool, "-v", spec.partlabel, partition] if tool else None
    if creator == "minix":
        tool = which_any("mkfs.minix")
        return [tool, "-3", partition] if tool else None
    if creator == "ufs":
        tool = which_any("mkfs.ufs")
        return [tool, partition] if tool else None
    if creator == "swap":
        tool = which_any("mkswap")
        return [tool, "-L", spec.partlabel, partition] if tool else None
    return None


def creator_status(spec: FilesystemSpec) -> tuple[str, str]:
    if spec.creator == "manual":
        return "manual", spec.note
    if spec.creator == "zfs":
        if which_any("zpool"):
            return "available", "zpool"
        hint = f" (package hint: {spec.package_hint})" if spec.package_hint else ""
        return "missing", f"zpool not found{hint}"
    command = creator_command(spec, "/dev/TEST")
    if command:
        return "available", os.path.basename(command[0])
    hint = f" (package hint: {spec.package_hint})" if spec.package_hint else ""
    return "missing", f"creator for {spec.key} not found{hint}"


def flatten_lsblk(nodes: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for node in nodes:
        result.append(node)
        result.extend(flatten_lsblk(node.get("children") or []))
    return result


def inspect_device(device: str) -> tuple[str, dict[str, Any], list[dict[str, Any]]]:
    canonical = os.path.realpath(device)
    try:
        mode = os.stat(canonical).st_mode
    except FileNotFoundError as exc:
        raise RuntimeError(f"target does not exist: {canonical}") from exc
    if not stat.S_ISBLK(mode):
        raise RuntimeError(f"target is not a block device: {canonical}")
    result = run([
        "lsblk", "--json", "--bytes", "--paths",
        "-o", "PATH,NAME,TYPE,SIZE,RM,RO,MODEL,SERIAL,TRAN,MOUNTPOINTS,PKNAME,PARTLABEL,FSTYPE,LABEL",
        canonical,
    ], capture=True)
    nodes = json.loads(result.stdout).get("blockdevices") or []
    if len(nodes) != 1:
        raise RuntimeError(f"could not uniquely inspect target: {canonical}")
    return canonical, nodes[0], flatten_lsblk(nodes)


def protected_system_disks() -> set[str]:
    protected: set[str] = set()
    for mountpoint in ("/", "/boot", "/boot/efi"):
        found = run(["findmnt", "-n", "-o", "SOURCE", "--target", mountpoint],
                    capture=True, check=False)
        if found.returncode != 0:
            continue
        source = found.stdout.strip()
        if not source.startswith("/dev/"):
            continue
        ancestry = run(["lsblk", "-s", "-n", "-p", "-o", "PATH,TYPE", source],
                       capture=True, check=False)
        for line in ancestry.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[-1] == "disk":
                protected.add(os.path.realpath(fields[0]))
    return protected


def validate_device_safety(canonical: str, root: dict[str, Any], *,
                           allow_non_removable: bool,
                           protected_disks: set[str]) -> None:
    if root.get("type") != "disk":
        raise RuntimeError(f"refusing non-whole-disk target {canonical!r} (TYPE={root.get('type')!r})")
    if bool(root.get("ro")):
        raise RuntimeError(f"refusing read-only target: {canonical}")
    if canonical in protected_disks:
        raise RuntimeError(f"refusing system/boot disk: {canonical}")
    size = int(root.get("size") or 0)
    if size < required_capacity_bytes():
        need = required_capacity_bytes() / GIB
        have = size / GIB
        raise RuntimeError(f"target is too small for the full harness layout: {have:.1f} GiB; need at least {need:.1f} GiB")
    removable = bool(root.get("rm"))
    transport = str(root.get("tran") or "").lower()
    if not allow_non_removable and not removable and transport not in {"usb", "mmc"}:
        raise RuntimeError(
            f"refusing non-removable target by default: {canonical} (RM={int(removable)}, TRAN={transport or 'unknown'}); "
            "use --allow-non-removable only after independently verifying the device"
        )


def mounted_descendants(flat: Sequence[dict[str, Any]]) -> list[tuple[str, str]]:
    mounts: list[tuple[str, str]] = []
    for node in flat:
        path = str(node.get("path") or "")
        for mountpoint in node.get("mountpoints") or []:
            if mountpoint:
                mounts.append((path, str(mountpoint)))
    return mounts


def print_device_summary(canonical: str, root: dict[str, Any], flat: Sequence[dict[str, Any]]) -> None:
    print(f"Target: {canonical}")
    print(f"Model:  {root.get('model') or 'unknown'}")
    print(f"Serial: {root.get('serial') or 'unknown'}")
    print(f"Size:   {int(root.get('size') or 0) / GIB:.1f} GiB")
    print(f"RM/TRAN: {int(bool(root.get('rm')))} / {root.get('tran') or 'unknown'}")
    mounts = mounted_descendants(flat)
    if mounts:
        print("Mounted descendants:")
        for path, mountpoint in mounts:
            print(f"  {path} -> {mountpoint}")
    else:
        print("Mounted descendants: none")


def print_layout_plan() -> None:
    print("\nPlanned GPT test layout:")
    print(f"{'#':>2}  {'Filesystem':<10} {'Part label':<12} {'Size':>7} {'Payload':>9}  Creator")
    for index, spec in enumerate(SPECS, 1):
        status, detail = creator_status(spec)
        payload = f"{spec.payload_mib} MiB" if spec.payload_mib else "n/a"
        print(f"{index:>2}  {spec.key:<10} {spec.partlabel:<12} {spec.size_mib:>5} MiB {payload:>9}  {status}: {detail}")
    print(f"\nAllocated test partitions: {sum(spec.size_mib for spec in SPECS) / 1024:.1f} GiB")
    print("The remaining card capacity is intentionally left unallocated.")


def default_state_file(device: str) -> Path:
    return STATE_ROOT / f"{Path(os.path.realpath(device)).name}.json"


def save_state(path: Path, state: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def load_state(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(f"harness state not found: {path}; run prepare first") from exc


def unmount_descendants(flat: Sequence[dict[str, Any]]) -> None:
    for _path, mountpoint in sorted(mounted_descendants(flat), key=lambda item: len(item[1]), reverse=True):
        run(["umount", mountpoint])


def settle_partitions(device: str) -> None:
    if shutil.which("partx"):
        run(["partx", "-u", device], check=False)
    if shutil.which("udevadm"):
        run(["udevadm", "settle"], check=False)
    time.sleep(0.5)


def partition_map(device: str) -> dict[str, str]:
    result = run(["lsblk", "--json", "--paths", "-o", "PATH,TYPE,PARTLABEL", device], capture=True)
    flat = flatten_lsblk(json.loads(result.stdout).get("blockdevices") or [])
    return {str(node["partlabel"]): str(node["path"])
            for node in flat if node.get("type") == "part" and node.get("partlabel")}


def testdata_command() -> list[str] | None:
    installed = shutil.which("linux-defragger-testdata")
    if installed:
        return [installed]
    local = Path(__file__).resolve().with_name("linux-defragger-testdata.py")
    if local.is_file():
        return [sys.executable, str(local)]
    return None


def mapper_command() -> list[str] | None:
    override = os.environ.get("LINUX_DEFRAGGER_ALLOCATION_MAPPER", "").strip()
    candidates = [Path(override)] if override else []
    candidates.extend([
        Path("/usr/lib/linux-defragger/allocation_mapper.py"),
        Path(__file__).resolve().parent.parent / "gui" / "allocation_mapper.py",
    ])
    for candidate in candidates:
        if candidate.is_file():
            return [sys.executable, str(candidate)]
    return None


def mount_partition(partition: str, mountpoint: Path, *, read_only: bool = False) -> None:
    mountpoint.mkdir(parents=True, exist_ok=True)
    command = ["mount"]
    if read_only:
        command += ["-o", "ro"]
    run(command + [partition, str(mountpoint)])


def read_manifest(mountpoint: Path) -> dict[str, Any]:
    return json.loads((mountpoint / DATA_DIR / "manifest.json").read_text(encoding="utf-8"))


def populate_regular(spec: FilesystemSpec, partition: str, work: Path) -> dict[str, Any]:
    command = testdata_command()
    if not command:
        raise RuntimeError("linux-defragger-testdata is not installed and source copy was not found")
    mountpoint = work / spec.key
    try:
        mount_partition(partition, mountpoint)
        run(command + [str(mountpoint)] + testdata_arguments(spec))
        return read_manifest(mountpoint)
    finally:
        run(["umount", str(mountpoint)], check=False)


def zfs_pool_name(device: str) -> str:
    digest = hashlib.sha256(os.path.realpath(device).encode("utf-8")).hexdigest()[:10]
    return f"ldtest_{digest}"


def populate_zfs(spec: FilesystemSpec, partition: str, work: Path,
                 device: str) -> tuple[dict[str, Any], str]:
    zpool = which_any("zpool")
    command = testdata_command()
    if not zpool or not command:
        raise RuntimeError("zpool and linux-defragger-testdata are required for ZFS population")
    pool = zfs_pool_name(device)
    existing = run([zpool, "list", "-H", "-o", "name", pool], capture=True, check=False)
    if existing.returncode == 0:
        raise RuntimeError(f"harness ZFS pool is already imported: {pool}; export it before retrying")
    altroot = work / "zfs-root"
    altroot.mkdir(parents=True, exist_ok=True)
    run([zpool, "create", "-f", "-R", str(altroot), "-m", "/ldtest",
         "-o", "cachefile=none", pool, partition])
    try:
        mountpoint = altroot / "ldtest"
        run(command + [str(mountpoint)] + testdata_arguments(spec))
        manifest = read_manifest(mountpoint)
    finally:
        run([zpool, "export", pool], check=False)
    return manifest, pool


def format_and_populate(spec: FilesystemSpec, partition: str, work: Path,
                        device: str) -> dict[str, Any]:
    record: dict[str, Any] = {
        "key": spec.key, "partlabel": spec.partlabel, "partition": partition,
        "size_mib": spec.size_mib, "payload_mib": spec.payload_mib,
        "expected_payload_bytes": target_payload_bytes(spec),
        "status": "pending", "manifest": None,
    }
    if spec.creator == "manual":
        record.update(status="skipped", reason=spec.note)
        return record
    if shutil.which("wipefs"):
        run(["wipefs", "--all", "--force", partition])
    if spec.creator == "zfs":
        if not which_any("zpool"):
            record.update(status="skipped", reason="zpool is not installed")
            return record
        try:
            manifest, pool = populate_zfs(spec, partition, work, device)
            record.update(status="populated", manifest=manifest, zfs_pool=pool)
        except Exception as exc:
            record.update(status="failed", reason=str(exc))
        return record
    command = creator_command(spec, partition)
    if not command:
        record.update(status="skipped", reason=creator_status(spec)[1])
        return record
    try:
        run(command)
        record["status"] = "formatted"
        if spec.payload_mib:
            try:
                record.update(status="populated", manifest=populate_regular(spec, partition, work))
            except Exception as exc:
                record.update(status="formatted-unpopulated",
                              reason=f"formatted successfully but population/mount failed: {exc}")
    except Exception as exc:
        record.update(status="failed", reason=str(exc))
    return record


def audit_records(device: str, state: dict[str, Any], phase: str) -> int:
    mapper = mapper_command()
    if not mapper:
        raise RuntimeError("allocation_mapper.py was not found; install Linux Defragger or run from the source tree")
    parts = partition_map(device)
    failures = 0
    for record in state.get("filesystems", []):
        partition = parts.get(record["partlabel"])
        if not partition or record.get("status") in {"skipped", "failed", "pending"}:
            continue
        print(f"Analysing {record['key']} on {partition}…", flush=True)
        result = run(mapper + [partition, "--cells", str(DEFAULT_CELLS)], capture=True, check=False)
        if result.returncode == 0:
            try:
                record[f"analysis_{phase}"] = json.loads(result.stdout)
            except json.JSONDecodeError:
                record[f"analysis_{phase}"] = {"error": "mapper returned non-JSON output", "stdout": result.stdout}
                failures += 1
        else:
            record[f"analysis_{phase}"] = {
                "error": (result.stderr or result.stdout).strip() or f"mapper exit {result.returncode}",
                "returncode": result.returncode,
            }
            failures += 1
    return failures


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_manifest(mountpoint: Path, expected: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    root = mountpoint / DATA_DIR
    for item in expected.get("target_files", []):
        path = root / item["path"]
        if not path.is_file():
            errors.append(f"missing file: {item['path']}")
            continue
        actual_size = path.stat().st_size
        if actual_size != int(item["size"]):
            errors.append(f"size mismatch: {item['path']}: {actual_size} != {item['size']}")
            continue
        if sha256_file(path) != item["sha256"]:
            errors.append(f"SHA-256 mismatch: {item['path']}")
    directory = root / "fragmented-directory"
    if not directory.is_dir():
        errors.append("missing fragmented-directory")
    else:
        expected_entries = int(expected.get("directory_entries", -1))
        if expected_entries >= 0:
            actual_entries = sum(1 for _ in directory.iterdir())
            if actual_entries != expected_entries:
                errors.append(f"directory entry count mismatch: {actual_entries} != {expected_entries}")
    return errors


def verify_regular(record: dict[str, Any], partition: str, work: Path) -> list[str]:
    mountpoint = work / record["key"]
    try:
        mount_partition(partition, mountpoint, read_only=True)
        return verify_manifest(mountpoint, record["manifest"])
    finally:
        run(["umount", str(mountpoint)], check=False)


def verify_zfs(record: dict[str, Any], partition: str, work: Path) -> list[str]:
    del partition
    zpool = which_any("zpool")
    if not zpool:
        raise RuntimeError("zpool is not installed")
    pool = record.get("zfs_pool")
    if not pool:
        raise RuntimeError("ZFS pool name missing from harness state")
    altroot = work / "zfs-verify"
    altroot.mkdir(parents=True, exist_ok=True)
    run([zpool, "import", "-f", "-R", str(altroot), "-o", "cachefile=none", pool])
    try:
        return verify_manifest(altroot / "ldtest", record["manifest"])
    finally:
        run([zpool, "export", pool], check=False)


def same_device_or_raise(canonical: str, root: dict[str, Any], state: dict[str, Any]) -> None:
    recorded = state.get("device", {})
    if os.path.realpath(recorded.get("path", "")) != canonical:
        raise RuntimeError(f"state belongs to {recorded.get('path')}, not {canonical}")
    if int(recorded.get("size") or 0) != int(root.get("size") or 0):
        raise RuntimeError("target size no longer matches prepared harness state")
    old_serial = str(recorded.get("serial") or "")
    new_serial = str(root.get("serial") or "")
    if old_serial and new_serial and old_serial != new_serial:
        raise RuntimeError("target serial no longer matches prepared harness state")


def ensure_root() -> None:
    if os.geteuid() != 0:
        raise RuntimeError("this command must be run as root (use sudo)")


def command_plan(args: argparse.Namespace) -> int:
    canonical, root, flat = inspect_device(args.device)
    validate_device_safety(canonical, root, allow_non_removable=args.allow_non_removable,
                           protected_disks=protected_system_disks())
    print_device_summary(canonical, root, flat)
    print_layout_plan()
    print(f"\nDestructive confirmation token for prepare: {confirmation_token(canonical)}")
    return 0


def initial_state(canonical: str, root: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": 1, "harness": "linux-defragger-media-harness",
        "device": {
            "path": canonical, "size": int(root.get("size") or 0),
            "model": root.get("model") or "", "serial": root.get("serial") or "",
            "transport": root.get("tran") or "", "removable": bool(root.get("rm")),
        },
        "layout": [asdict(spec) for spec in SPECS],
        "created_at": int(time.time()), "filesystems": [],
    }


def command_prepare(args: argparse.Namespace) -> int:
    ensure_root()
    canonical, root, flat = inspect_device(args.device)
    validate_device_safety(canonical, root, allow_non_removable=args.allow_non_removable,
                           protected_disks=protected_system_disks())
    print_device_summary(canonical, root, flat)
    print_layout_plan()
    token = confirmation_token(canonical)
    print("\nWARNING: PREPARE DESTROYS THE ENTIRE TARGET DISK AND ALL EXISTING PARTITIONS.")
    if input(f"Type exactly: {token}\n> ").strip() != token:
        raise RuntimeError("destructive confirmation did not match; nothing was changed")

    canonical2, root2, flat2 = inspect_device(canonical)
    if canonical2 != canonical or int(root2.get("size") or 0) != int(root.get("size") or 0):
        raise RuntimeError("target changed after confirmation; refusing to continue")
    old_serial = str(root.get("serial") or "")
    new_serial = str(root2.get("serial") or "")
    if old_serial and new_serial and old_serial != new_serial:
        raise RuntimeError("target serial changed after confirmation; refusing to continue")
    validate_device_safety(canonical2, root2, allow_non_removable=args.allow_non_removable,
                           protected_disks=protected_system_disks())

    unmount_descendants(flat2)
    state_path = Path(args.state_file) if args.state_file else default_state_file(canonical)
    state = initial_state(canonical, root2)
    save_state(state_path, state)
    run(["sfdisk", "--wipe", "always", "--lock", canonical], input_text=sfdisk_script())
    settle_partitions(canonical)
    parts = partition_map(canonical)
    missing_labels = [spec.partlabel for spec in SPECS if spec.partlabel not in parts]
    if missing_labels:
        raise RuntimeError(f"partition table was written but expected partitions are missing: {', '.join(missing_labels)}")

    with tempfile.TemporaryDirectory(prefix="linux-defragger-media-harness-") as temporary:
        work = Path(temporary)
        for spec in SPECS:
            partition = parts[spec.partlabel]
            print(f"\n=== {spec.key}: {partition} ===", flush=True)
            record = format_and_populate(spec, partition, work, canonical)
            state["filesystems"].append(record)
            save_state(state_path, state)
            reason = record.get("reason", "")
            print(f"{spec.key}: {record['status']}{': ' + reason if reason else ''}")

    print("\nRunning Linux Defragger read-only analysis on the prepared, unmounted partitions…")
    audit_failures = audit_records(canonical, state, "before")
    save_state(state_path, state)
    print_summary(state)
    print(f"\nHarness state: {state_path}")
    print("Next: open Linux Defragger and test Analyse/Defragment on the unmounted partitions.")
    print(f"Afterwards run: sudo linux-defragger-media-harness audit {canonical} --phase after")
    print(f"Then run:       sudo linux-defragger-media-harness verify {canonical}")
    return 1 if audit_failures else 0


def command_audit(args: argparse.Namespace) -> int:
    ensure_root()
    canonical, root, _flat = inspect_device(args.device)
    state_path = Path(args.state_file) if args.state_file else default_state_file(canonical)
    state = load_state(state_path)
    same_device_or_raise(canonical, root, state)
    failures = audit_records(canonical, state, args.phase)
    save_state(state_path, state)
    print_summary(state)
    print(f"Audit saved to {state_path}")
    return 1 if failures else 0


def command_verify(args: argparse.Namespace) -> int:
    ensure_root()
    canonical, root, _flat = inspect_device(args.device)
    state_path = Path(args.state_file) if args.state_file else default_state_file(canonical)
    state = load_state(state_path)
    same_device_or_raise(canonical, root, state)
    parts = partition_map(canonical)
    failures = 0
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="linux-defragger-media-verify-") as temporary:
        work = Path(temporary)
        for record in state.get("filesystems", []):
            if not record.get("manifest"):
                continue
            partition = parts.get(record["partlabel"])
            if not partition:
                print(f"VERIFY FAIL {record['key']}: partition {record['partlabel']} missing")
                failures += 1
                continue
            try:
                errors = (verify_zfs(record, partition, work) if record["key"] == "zfs"
                          else verify_regular(record, partition, work))
            except Exception as exc:
                print(f"VERIFY SKIP {record['key']}: could not mount/import for checksum verification: {exc}")
                skipped += 1
                continue
            if errors:
                failures += 1
                print(f"VERIFY FAIL {record['key']}:")
                for error in errors:
                    print(f"  {error}")
            else:
                print(f"VERIFY OK   {record['key']}: target files and directory manifest match")
    print(f"\nVerification result: {failures} failed, {skipped} skipped because the host could not mount/import them.")
    return 1 if failures else 0


def print_summary(state: dict[str, Any]) -> None:
    print("\nField-test media summary:")
    for record in state.get("filesystems", []):
        before = record.get("analysis_before")
        after = record.get("analysis_after")
        bits = [record["status"]]
        if before is not None:
            bits.append("analysis-before=" + ("ok" if "error" not in before else "error"))
        if after is not None:
            bits.append("analysis-after=" + ("ok" if "error" not in after else "error"))
        print(f"  {record['key']:<10} {record['partlabel']:<12} {' '.join(bits)}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Destructive physical-media field-test harness for Linux Defragger"
    )
    parser.add_argument("--state-file", default="", help="override the host-side JSON state path")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "prepare"):
        command = subparsers.add_parser(name)
        command.add_argument("device", help="whole-disk block device, e.g. /dev/mmcblk0")
        command.add_argument("--allow-non-removable", action="store_true",
                             help="allow a disk not reported removable/USB/MMC; system disks are still refused")
    audit = subparsers.add_parser("audit")
    audit.add_argument("device", help="prepared whole-disk block device")
    audit.add_argument("--phase", choices=("before", "after"), default="after")
    verify = subparsers.add_parser("verify")
    verify.add_argument("device", help="prepared whole-disk block device")
    subparsers.add_parser("layout", help="show the filesystem layout without inspecting a device")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "layout":
            print_layout_plan()
            return 0
        if args.command == "plan":
            return command_plan(args)
        if args.command == "prepare":
            return command_prepare(args)
        if args.command == "audit":
            return command_audit(args)
        if args.command == "verify":
            return command_verify(args)
        parser.error(f"unknown command: {args.command}")
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"linux-defragger-media-harness: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
