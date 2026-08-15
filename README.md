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

Unsupported on-disk layouts fail closed rather than being guessed. The exact writers perform a final read-only rescan before reporting success.

## Raw userspace design

The writer engines use ordinary Linux raw block I/O but do not depend on Linux filesystem-driver support for physical placement. This is intentional: for example, an XFS target can be analysed and rewritten by the XFS engine without mounting XFS or asking the XFS kernel driver to relocate files.

Persistent staged writers build and verify a private working image before committing authoritative changes to the source. XFS, EXT, exFAT, HFS+/HFSX and Amiga OFS/FFS commit verified allocated/metadata ranges rather than rewriting free address space merely because the stage spans the full logical filesystem. Recovery re-verifies the persistent stage before resuming writes.

The production architecture test rejects known external filesystem mutation/repair command orchestration and loop/mount delegation.

## No bundled third-party source

The source repository contains **no vendored third-party source tree**. Required libraries are supplied by the host distribution at build/runtime rather than copied into Linux Defragger. The project itself is licensed `GPL-3.0-or-later` and first-party source files carry SPDX identifiers.

## Linux Defragger Test Media

The package includes **Linux Defragger Test Media**, a separate all-C GTK application for building and verifying sacrificial filesystem test disks. It is not a window inside Linux Defragger and it does not use the Python GUI or the former Python field-test harness. Launch **Linux Defragger Test Media** from the desktop application menu; the executable is `linux-defragger-test-media`.

The GUI lists physical disks with model, serial, size and transport, marks the system/boot disk as protected, and only enables destructive preparation for a sufficiently large removable, USB or MMC field-media target. Before changing a disk it shows the exact selected device in a destructive confirmation dialog, and the privileged C worker repeats the whole-disk, system-disk, read-only, size and transport checks after privilege elevation.

The Test Media window is responsive rather than fixed around one desktop size. The filesystem table and live operation log share a draggable vertical splitter and grow with the window. Creator availability and operation result are separate columns so a reserved or unavailable creator is not confused with a formatting/verification failure. The header shows an at-a-glance readiness count, the progress bar reports completed filesystem slots, and each row exposes explanatory detail as a tooltip.

Creator states are intentionally explicit. **Ready** means the required creator is available on the current host or is supplied by Test Media itself. **Optional package missing** means Test Media knows the distribution package normally providing the creator. **No standard creator** means no supported creator/package path is claimed for that slot, so it remains reserved while the rest of the build continues. **Reserved / manual** is used where automatic creation is deliberately not attempted. Test Media never writes a fake filesystem signature merely to make a row appear successful.

The test layout contains 21 dedicated slots: FAT12, FAT16, FAT32, exFAT, NTFS, ext2, ext3, ext4, XFS, Btrfs, Amiga OFS, Amiga FFS, Amiga SFS/SFS2, Amiga PFS3, classic HFS, HFS+, Minix, UFS, ZFS, APFS and Swap. OFS and FFS are separate real test filesystems created as Amiga `DOS\\0` and `DOS\\1`. Their formatting, directory/file metadata construction, data-block allocation, deliberate fragmentation and post-defrag verification are all performed directly by first-party C against the raw partition; Test Media does not need the Linux AFFS kernel driver to mount either filesystem. The production Amiga parser independently traverses the generated structures, and the normal profile requires every retained target file to contain at least 100 physical fragments. SFS/SFS2 and PFS3 remain separate reserved roadmap slots because Linux Defragger does not yet implement those filesystem engines or creators. APFS likewise remains a real reserved partition rather than receiving a fake signature.

UFS test media is manufactured as a genuine little-endian UFS2/FFS image using the distribution `makefs` utility from a deterministic 200 MiB test tree. Before copying that image to the `LD_UFS` partition, and again after copying it, Test Media requires Linux Defragger's independent native UFS parser to recognise it as UFS2. The current production UFS analyser is summary-level and does not yet decode inode/block allocation deeply enough to prove an exact fragmentation count, so Test Media deliberately does **not** claim a specific UFS fragmentation level yet.

Normal populated filesystems receive about 200 MiB of deterministic test data. FAT12 deliberately uses a smaller 4 MiB target and a scaled directory-fragmentation profile so the 64 MiB FAT12 test volume remains valid. Fragmentation generation, SHA-256 manifest creation, state recording and post-defrag verification are implemented in C. **Verify After Defrag** verifies OFS/FFS directly through the raw C engine; for the other previously populated filesystems it mounts/imports read-only where the host supports doing so and verifies retained file sizes, SHA-256 hashes and directory-entry counts. A host that cannot mount a filesystem reports it as unverified rather than corrupt. Host-side state is stored under `/var/tmp/linux-defragger-test-media/`.

Formatting and mounting utilities such as `mkfs.fat`, `mkfs.xfs`, `hformat`, `makefs` and `zpool` are allowed here because this companion exists solely to manufacture hostile test media. The OFS and FFS formatter/population path is first-party raw C rather than an external filesystem utility. Linux Defragger's production analyser and writer engines remain subject to the no-mount/no-external-filesystem-mutation architecture rule.

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
