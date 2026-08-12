<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger 1.8.0-95

Linux Defragger provides direct allocation analysis and offline canonical layout rewriting for supported filesystems. Writers require the selected target and any genuinely overlapping block mappings to be unmounted; mounted sibling partitions do not block partition-level operations.

## Licence

Linux Defragger first-party code, scripts, tests, packaging and documentation are licensed under **GNU GPL version 3 or any later version** (`GPL-3.0-or-later`). First-party source files carry SPDX licence identifiers, with sidecar `.license` files used only where a file format cannot safely contain comments. The canonical GPLv3 text is provided in `LICENSES/GPL-3.0-or-later.txt`.

The remaining bundled classic-HFS `hfsutils` source is third-party code and retains its original GPL-2.0-or-later terms; see `THIRD_PARTY_NOTICES.md`. It is not part of the first-party relicensing.

## Current support

| Filesystem | Analyse | Defragment | Growth Defrag | Recover |
|---|---:|---:|---:|---:|
| FAT12/16/32 | Yes | Native C | Exact 10% | Yes |
| exFAT | Yes | Native C | Exact 10% | Yes |
| NTFS | Yes | Native C, fail-closed preflight | Exact 10% | Yes |
| ext2/ext3/ext4 | Yes | Native C staged writer | Exact 10% | Yes |
| XFS v5 | Yes | Native C raw userspace, restricted preflight | Exact 10% | Yes |
| Amiga OFS/FFS | Yes | Native C | Exact 10% | Yes |
| HFS+/HFSX | Yes | Native C, fail-closed preflight | Exact 10% | Yes |
| Btrfs, classic HFS, APFS, Minix, UFS, ZFS, swap | Recognition or analysis | Not implemented | Not implemented | No |

## Architecture

There is one filesystem hierarchy: `gui/filesystems/<format>/`. `gui/backends/registry.py` is the sole filesystem registry. Write-capable filesystem logic lives in first-party C below the owning package's `native/` directory; Python `plugin.py` files are GUI/backend adapters only. Filesystem-neutral C raw-I/O, device-safety, staging and Stop services remain in `src/core/`.

The production architecture test rejects external filesystem mutation/repair command orchestration. Writers use direct raw I/O and may link fixed userspace libraries in-process, but they do not mount a filesystem to ask its kernel driver to choose physical placement.

## Staged writers and commit I/O

XFS, EXT, exFAT, HFS+/HFSX and Amiga OFS/FFS build and verify a persistent private working image before source commit. Their commit paths write verified allocated/metadata ranges rather than rewriting free address space merely because the sparse stage spans the full logical filesystem. Recovery re-verifies the persistent stage before resuming source writes.

## HFS+/HFSX native writer

Revision 94 replaces the read-only Python HFS+/HFSX implementation with a first-party C engine under `gui/filesystems/hfsplus/native/`. The engine parses the volume header, allocation file, catalog B-tree and extents-overflow B-tree directly. User data/resource forks are packed around fixed filesystem metadata; catalog and existing overflow extent descriptors are updated without rebuilding B-tree topology. Growth Defrag leaves an exact 10% free allocation-block reserve after each movable fork.

Journaled volumes are accepted only when their internal journal is demonstrably empty (or explicitly marked as not yet initialised). Journal info/data blocks remain fixed. A journal containing pending transactions is rejected before mutation because revision 94 deliberately does not replay HFS+ journal transactions.

## XFS raw userspace writer

The XFS writer does not mount XFS, create a loop-mounted XFS stage, issue XFS filesystem ioctls, use FIEMAP for placement, or invoke xfsprogs in production. It performs raw allocation-group, inode, B-tree, reverse-mapping, CRC and journal-cleanliness work itself. Unsupported layouts fail closed before source commit.

## Safety model

- writable block targets use exclusive raw opening;
- overlap-aware mount checks reject mounted targets/mappings without rejecting disjoint sibling partitions;
- persistent staged writers keep source bytes unchanged until the private image verifies;
- recovery re-verifies the stage before authoritative source writes;
- unsupported on-disk layouts abort before source commit;
- every successful mutation performs a final read-only verification.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./tests/run_tests.sh ./build/linux-defragger-fat-worker
```

## Release files

Revision 95 uses three artifacts:

| File | Purpose |
|---|---|
| `linux-defragger_1.8.0-95_amd64.deb` | Generic Debian-managed amd64 build (`-march=x86-64 -mtune=generic`). |
| `linux-defragger-1.8.0-95-local-folder.run` | Self-contained compiler/installer using `-march=native -mtune=native`; installs `1.8.0-95+native1`. |
| `linux-defragger-1.8.0-95-local-source.zip` | Complete source, build system, tests, resources and documentation. |

Build them with `packaging/build-deb.sh`, `packaging/build-local-run.sh`, and `packaging/build-source-zip.sh`. `VERSION` is the release source for generated C/Python/Debian metadata.

## Reconstruction provenance

This line descends from the reconstructed revision-80 source after the original revision-80 working directory was lost before packaging. It does not claim byte-for-byte preservation of unrelated unrecoverable revision-63-through-79 changes.
