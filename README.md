<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Linux Defragger

[![Project quality gate](https://github.com/Infiltrator-Projects/Defragger/actions/workflows/quality-gate.yml/badge.svg)](https://github.com/Infiltrator-Projects/Defragger/actions/workflows/quality-gate.yml)

Linux Defragger is a C-first offline filesystem allocation analyser and defragmenter for Linux. Write-capable engines operate directly on unmounted block devices or filesystem images and do not delegate production mutations to mounted kernel filesystem drivers or external repair/defragmentation tools.

**Current version:** 1.8.0-141

**Platform:** Linux

**Licence:** GPL-3.0-or-later

> **Safety status:** The version 1.8.0-141 filesystem-safety audit is complete. Defragment, Growth Defrag and Recover are enabled behind exact target confirmation, mounted-target refusal, durable filesystem-specific recovery and final verification. The separate Test Media utility is deliberately destructive and must be used only on sacrificial targets. See `defragger/docs/AUDIT_STATUS.md`.

## Capabilities

The project contains native analysis support across FAT12/16/32, exFAT, NTFS, ext2/3/4, XFS v5, Amiga OFS/FFS/SFS variants, HFS/HFS+, Btrfs, APFS, Minix, UFS, ZFS/OpenZFS members and Linux swap, with write support implemented only where the filesystem-specific engine has an explicit contract.

Production operations are:

- **Analyse / Map** — read-only allocation and fragmentation analysis.
- **Defragment** — canonical placement of supported movable allocations.
- **Growth Defrag** — canonical placement with an exact 10% post-file reserve.
- **Recover** — resume/repair of supported interrupted persistent transactions.

Unsupported layouts fail closed rather than being guessed. Write-capable engines perform verified staging/recovery and a final read-only rescan before reporting success.

## Architecture

The canonical implementation lives under `defragger/`.

Filesystem implementations are organised below `defragger/gui/filesystems/<format>/`, with native C under `native/` where exact low-level analysis or mutation is required. Filesystem-neutral device safety, raw I/O, Stop handling and shared runtime support live under `defragger/src/core/`.

The operating system supplies raw block I/O, but filesystem parsing, placement planning, staging and metadata updates are owned by the project. Architecture and regression tests reject known external filesystem mutation/repair orchestration and duplicate implementation paths.

Shared first-party primitives are consumed from the pinned Infiltratr Common dependency; filesystem-specific rules remain in Defragger.

## Build and test

From the canonical project directory:

```bash
cd defragger
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The permanent GitHub quality gate performs a warnings-as-errors C build and runs the complete native, filesystem, GUI, architecture, safety and release regression suite.

## Release assets

A numbered release publishes:

| File | Purpose |
| --- | --- |
| `linux-defragger_<version>_amd64.deb` | Generic amd64 Debian package. |
| `linux-defragger-<version>-local-folder.run` | Hardware-native local compile/install program. |
| `Defragger-<version>.zip` | Tested source archive from the exact release commit. |
| `RELEASE_SHA256SUMS.txt` | SHA-256 checksums for published artifacts. |

## Repository and release policy

This repository uses `main` as its working branch. Development changes are made directly on `main`; the normal project workflow does not depend on PR, feature or release branches.

Every push to `main` runs the project quality gate. Ordinary commits do not publish. A commit becomes release-eligible only when its subject begins `Release <version>` and the complete quality gate succeeds.

The release workflow checks the exact tested `main` commit, confirms that `main` is permanently protected by the `quality-gate` status check, and creates a new immutable version tag and release only from that exact current tested commit.

Existing version tags and published releases are immutable and are never moved, replaced or edited in place. Manually runnable quality-gate helpers are diagnostic tools only and are not release-approval mechanisms.

## Documentation

- `defragger/README.md` — detailed project manual and filesystem-support matrix.
- `defragger/docs/AUDIT_STATUS.md` — current safety-audit status.
- `defragger/docs/DESIGN.md` — technical architecture and filesystem contracts.
- `defragger/VERSION` — current source version.

## Safety

Defragmentation changes filesystem allocation metadata and data placement. Keep verified backups and use sacrificial/test media during development and validation. The **Linux Defragger Test Media** utility is intentionally destructive and must never be pointed at a system/boot disk or irreplaceable media.

## Licence

Copyright © 2026 Shannon Smith.

Linux Defragger first-party code, scripts, tests, packaging and documentation are licensed under the GNU General Public License version 3 or, at your option, any later version (`GPL-3.0-or-later`). The canonical licence text is `LICENSE`.
