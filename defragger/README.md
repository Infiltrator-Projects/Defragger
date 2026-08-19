<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger

Linux Defragger is a C-first, offline filesystem allocation analyser and defragmenter for Linux. Write-capable engines operate directly on unmounted block devices or filesystem images. They do not mount the target, ask the kernel filesystem driver to choose physical placement, or launch external filesystem repair/defragmentation tools to perform production mutations.

The current software version is defined by [`defragger/VERSION`](defragger/VERSION). Installable builds are published on the repository's Releases page.

> **Important:** defragmentation changes filesystem allocation metadata and data placement. Use verified backups and test media before using write-capable operations on important filesystems.

## Filesystem support

| Filesystem | Analyse / Map | Defragment | Growth Defrag | Recover |
|---|---|---|---|---|
| FAT12 / FAT16 / FAT32 | Exact | Native C | Native C, exact 10% reserve | Yes |
| exFAT | Exact | Native C | Native C, exact 10% reserve | Yes |
| NTFS | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| ext2 / ext3 / ext4 | Exact | Native C staged writer | Native C, exact 10% reserve | Yes |
| XFS v5 | Exact | Native C raw userspace writer | Native C, exact 10% reserve | Yes |
| Amiga OFS / FFS | Exact | Native C | Native C, exact 10% reserve | Yes |
| Amiga SFS / SFS2 | Not implemented | Not implemented | Not implemented | No |
| Amiga PFS3 | Not implemented | Not implemented | Not implemented | No |
| HFS+ / HFSX | Exact | Native C, fail-closed preflight | Native C, exact 10% reserve | Yes |
| Classic Macintosh HFS | Exact, native C read-only | Not implemented | Not implemented | No |
| Btrfs | Exact read-only raw analysis | Not implemented | Not implemented | No |
| APFS | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| Minix | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| UFS | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| ZFS / OpenZFS member | Summary read-only analysis, native C | Not implemented | Not implemented | No |
| Linux swap | Exact inactive / aggregate active read-only analysis, native C | Not applicable | Not applicable | No |

Unsupported on-disk layouts fail closed rather than being guessed. Exact writers perform a final read-only rescan before reporting success.

## Design

Linux Defragger is intentionally filesystem-driver independent for placement work. The operating system still provides ordinary raw device I/O, but filesystem parsing, allocation planning, staging and metadata updates are owned by the project rather than delegated to the mounted kernel filesystem implementation.

Each filesystem has one authoritative implementation under `defragger/gui/filesystems/<format>/`, with native C under `native/` where exact low-level analysis or mutation is implemented. Filesystem-neutral device safety, raw I/O, Stop handling and shared runtime support live under `defragger/src/core/`.

Persistent write-capable engines use verified staging and recovery state before authoritative source changes. The architecture and regression tests reject known external filesystem mutation/repair orchestration and duplicate implementation paths.

The repository does not vendor third-party source trees. Shared first-party functionality is consumed through the pinned Infiltratr Common dependency, while required system libraries are supplied by the host distribution.

For the detailed technical contract, see [`defragger/docs/DESIGN.md`](defragger/docs/DESIGN.md).

## Operations

Linux Defragger exposes three production operations:

- **Defragment** — places supported movable allocations into the earliest legal canonical layout.
- **Growth Defrag** — uses the same canonical placement model while reserving exactly 10% free space immediately after each regular file.
- **Recover** — resumes or repairs supported interrupted persistent transactions when the filesystem-specific recovery contract permits it.

Stop requests are honoured at filesystem-safe transaction boundaries rather than by abandoning an authoritative write mid-transaction.

## Test Media

The package includes **Linux Defragger Test Media**, a separate all-C GTK utility for preparing sacrificial test disks. It can use removable media or a dedicated secondary fixed disk, while protecting the system/boot disk and repeating destructive-target checks after privilege elevation.

The standard test layout provides dedicated slots for FAT12, FAT16, FAT32, exFAT, NTFS, ext2, ext3, ext4, XFS, Btrfs, Amiga OFS, Amiga FFS, Amiga SFS/SFS2, Amiga PFS3, classic HFS, HFS+, Minix, UFS, ZFS, APFS and Swap. Unsupported creator/engine combinations remain explicitly reserved rather than receiving fake filesystem signatures.

Formatting utilities are permitted inside Test Media solely to manufacture hostile test filesystems. Production analyser and writer engines remain subject to the raw-userspace/no-external-mutation rule.

## Build and test

```bash
cmake -S defragger -B defragger/build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build defragger/build -j"$(nproc)"
ctest --test-dir defragger/build --output-on-failure
```

The permanent GitHub quality gate performs a warnings-as-errors C build and runs the complete native, filesystem, GUI, architecture, safety and release regression suite.

## Release downloads

Each numbered release uses three project deliverables:

| File | Purpose |
|---|---|
| `linux-defragger_${VERSION}_amd64.deb` | Generic amd64 Debian package (`-march=x86-64 -mtune=generic`). |
| `linux-defragger-${VERSION}-local-folder.run` | Hardware-optimised local compile-and-install program (`-march=native -mtune=native`). |
| `Defragger-${VERSION}.zip` | Clean source archive built and tested from the exact release commit. |

Release publication is gated: the release workflow only publishes the requested version after the reusable project quality gate succeeds. GitHub's automatic tag source links may also be displayed, but `Defragger-${VERSION}.zip` is the project's verified source deliverable.

## Repository layout

The repository root is intentionally minimal. All implementation, native engines, CMake modules, tests, test-media code, packaging, design documentation and pinned shared dependency live under [`defragger/`](defragger/).

## Licence

Copyright © 2026 Shannon Smith.

Linux Defragger first-party code, scripts, tests, packaging and documentation are licensed under the **GNU General Public License version 3 or, at your option, any later version** (`GPL-3.0-or-later`). The canonical licence text is [`LICENSE`](LICENSE).
