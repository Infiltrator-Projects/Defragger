<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger

Linux Defragger is a C-first, offline filesystem allocation analyser and defragmenter for Linux. Write-capable engines operate directly on an unmounted block device or image: they do not mount the target, ask the kernel filesystem driver to choose physical placement, or launch external filesystem repair/defragmentation utilities.

The project is deliberately modular. Each filesystem owns one authoritative implementation under `gui/filesystems/<format>/`, with native C beneath `native/` where mutation or exact low-level analysis is implemented. Filesystem-neutral raw I/O, staging, device safety and Stop handling live in `src/core/`.

The authoritative current software version is the root [`VERSION`](VERSION) file. Published installable builds are available from the repository's **Latest Release**. This README intentionally does not embed a release number so it cannot become stale when a new version is published.

## Current filesystem support

| Filesystem | Analyse / Map | Defragment | Growth Defrag | Recover |
|---|---|---|---|---|
| FAT12 / FAT16 / FAT32 | Exact | Native C | Native C, exact 10% reserve | Yes |
| exFAT | Exact | Native C | Native C, exact 10% reserve | Yes |
| NTFS | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| ext2 / ext3 / ext4 | Exact | Native C staged writer | Native C, exact 10% reserve | Yes |
| XFS v5 | Exact | Native C raw userspace writer | Native C, exact 10% reserve | Yes |
| Amiga OFS / FFS | Exact | Native C | Native C, exact 10% reserve | Yes |
| HFS+ / HFSX | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| Classic Macintosh HFS | Exact, native C read-only | Not implemented | Not implemented | No |
| Btrfs | Exact read-only raw analysis | Not implemented | Not implemented | No |
| APFS | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| Minix | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| UFS | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| ZFS / OpenZFS member | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| Linux swap | Exact inactive / aggregate active read-only analysis, native C | Not applicable | Not applicable | No |

Unsupported on-disk layouts fail closed rather than being guessed. The exact writers perform a final read-only rescan before reporting success.

## Raw userspace design

The writer engines use ordinary Linux raw block I/O but do not depend on Linux filesystem-driver support for physical placement. This is intentional: for example, an XFS target can be analysed and rewritten by the XFS engine without mounting XFS or asking the XFS kernel driver to relocate files.

Persistent staged writers build and verify a private working image before committing authoritative changes to the source. XFS, EXT, exFAT, HFS+/HFSX and Amiga OFS/FFS commit verified allocated/metadata ranges rather than rewriting free address space merely because the stage spans the full logical filesystem. Recovery re-verifies the persistent stage before resuming writes.

The production architecture test rejects known external filesystem mutation/repair command orchestration and loop/mount delegation.

## No bundled third-party source

The source repository contains **no vendored third-party source tree**. Required libraries are supplied by the host distribution at build/runtime rather than copied into Linux Defragger. The project itself is licensed `GPL-3.0-or-later` and first-party source files carry SPDX identifiers.

## Physical-media field-test harness

The installed `linux-defragger-media-harness` utility can destructively prepare a dedicated removable test disk with GPT slots for every filesystem backend. Where the host has a genuine creator and writable mount path, it formats the partition and uses `linux-defragger-testdata` to leave deterministic fragmented data behind. Normal populated filesystems retain about 200 MiB of target-file payload; FAT12 uses a deliberately smaller profile; Swap contains no files; unsupported creators such as APFS are reported as skipped rather than faked.

The harness rejects partitions instead of whole disks, refuses the system/boot disk, rejects undersized targets, and requires an explicit override for a disk not reported as removable/USB/MMC. `prepare` also requires typing the exact `DESTROY /dev/...` token after displaying the device model, serial, size, mount state and full partition plan.

```bash
sudo linux-defragger-media-harness plan /dev/mmcblk0
sudo linux-defragger-media-harness prepare /dev/mmcblk0
# Test Analyse / Defragment in the GUI while the test partitions are unmounted.
sudo linux-defragger-media-harness audit /dev/mmcblk0 --phase after
sudo linux-defragger-media-harness verify /dev/mmcblk0
```

`prepare` stores a host-side JSON record under `/var/tmp/linux-defragger-media-harness/`, including each generated SHA-256 manifest and the pre-test Linux Defragger allocation map. `audit --phase after` records a second map after field testing, while `verify` mounts/imports only the filesystems that were successfully populated and checks the retained file sizes, SHA-256 digests and directory-entry count. If the host kernel cannot mount a filesystem, the harness records that limitation rather than treating it as filesystem corruption.

## Build

A normal development build is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The native `.run` installer installs required build/runtime packages, compiles for the current machine using `-march=native -mtune=native`, builds a Debian package and installs it through `dpkg`.

## Release files

Each numbered release uses the standard three-download model. For the version stored in `VERSION`, the release consists of:

| File | Purpose |
|---|---|
| `linux-defragger_${VERSION}_amd64.deb` | Generic amd64 Debian package (`-march=x86-64 -mtune=generic`). |
| `linux-defragger-${VERSION}-local-folder.run` | Hardware-optimised local compile-and-install program. |
| `Defragger-${VERSION}.zip` | Canonical GitHub source archive from the exact matching version tag. |

The generic package and native installer are built with `packaging/build-deb.sh` and `packaging/build-local-run.sh`. `packaging/build-source-zip.sh` produces the same canonical `Defragger-${VERSION}.zip` name and top-level directory when a local source archive is required.

## Documentation

`docs/DESIGN.md` is the single technical design document. Release history lives in Git tags/releases, and the executable test suite is the authority for regression status.

## Licence

Copyright © 2026 Shannon Smith.

Linux Defragger first-party code, scripts, tests, packaging and documentation are licensed under the **GNU General Public License version 3 or, at your option, any later version** (`GPL-3.0-or-later`). The canonical licence text is the root `LICENSE` file.
