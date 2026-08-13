<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger 1.8.0-96

Linux Defragger is a C-first, offline filesystem allocation analyser and defragmenter for Linux. Write-capable engines operate directly on an unmounted block device or image: they do not mount the target, ask the kernel filesystem driver to choose physical placement, or launch external filesystem repair/defragmentation utilities.

The project is deliberately modular. Each filesystem owns one authoritative implementation under `gui/filesystems/<format>/`, with native C beneath `native/` where mutation or exact low-level analysis is implemented. Filesystem-neutral raw I/O, staging, device safety and Stop handling live in `src/core/`.

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
| APFS | Summary read-only analysis | Not implemented | Not implemented | No |
| Minix | Summary read-only analysis | Not implemented | Not implemented | No |
| UFS | Summary read-only analysis | Not implemented | Not implemented | No |
| ZFS / OpenZFS member | Summary read-only analysis | Not implemented | Not implemented | No |
| Linux / Solaris swap | Summary read-only analysis | Not applicable | Not applicable | No |

Unsupported on-disk layouts fail closed rather than being guessed. The exact writers perform a final read-only rescan before reporting success.

## Raw userspace design

The writer engines use ordinary Linux raw block I/O but do not depend on Linux filesystem-driver support for physical placement. This is intentional: for example, an XFS target can be analysed and rewritten by the XFS engine without mounting XFS or asking the XFS kernel driver to relocate files.

Persistent staged writers build and verify a private working image before committing authoritative changes to the source. XFS, EXT, exFAT, HFS+/HFSX and Amiga OFS/FFS commit verified allocated/metadata ranges rather than rewriting free address space merely because the stage spans the full logical filesystem. Recovery re-verifies the persistent stage before resuming writes.

The production architecture test rejects known external filesystem mutation/repair command orchestration and loop/mount delegation.

## No bundled third-party source

The source repository contains **no vendored third-party source tree**. Required libraries are supplied by the host distribution at build/runtime rather than copied into Linux Defragger. The project itself is licensed `GPL-3.0-or-later` and first-party source files carry SPDX identifiers.

## Build

A normal development build is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The native `.run` installer installs required build/runtime packages, compiles for the current machine using `-march=native -mtune=native`, builds a Debian package and installs it through `dpkg`.

## Release files

Linux Defragger 1.8.0-96 uses the standard three-download model:

| File | Purpose |
|---|---|
| `linux-defragger_1.8.0-96_amd64.deb` | Generic amd64 Debian package (`-march=x86-64 -mtune=generic`). |
| `linux-defragger-1.8.0-96-local-folder.run` | Hardware-optimised local compile-and-install program. |
| `Defragger-1.8.0-96.zip` | Canonical GitHub source archive from the exact `v1.8.0-96` tag. |

The generic package and native installer are built with `packaging/build-deb.sh` and `packaging/build-local-run.sh`. `packaging/build-source-zip.sh` produces the same canonical `Defragger-${VERSION}.zip` name and top-level directory when a local source archive is required.

## Documentation

- `RELEASE_NOTES.md` — revision history and behavioural changes.
- `TEST_STATUS.md` — current regression coverage and validation gates.
- `docs/DESIGN.md` — architecture, canonical layout and transaction design.
- `docs/FILESYSTEM_PLUGIN_CONTRACT.md` — filesystem plugin/worker contract.
- `docs/COPYRIGHT.md` — copyright and licensing statement.

## Licence

Copyright © 2026 Shannon Smith.

Linux Defragger first-party code, scripts, tests, packaging and documentation are licensed under the **GNU General Public License version 3 or, at your option, any later version** (`GPL-3.0-or-later`). The canonical licence text is in `LICENSES/GPL-3.0-or-later.txt`.
