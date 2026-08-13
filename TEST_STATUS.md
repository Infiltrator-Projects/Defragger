<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Test status — 1.8.0-96

The first-party C build is required to compile with `LD_ENABLE_WERROR=ON`.

Current focused regressions cover:

- project-wide `GPL-3.0-or-later` SPDX coverage, including `.license` sidecars for non-commentable first-party artefacts;
- a no-vendored-source architecture invariant;
- FAT12/FAT16/FAT32 Defrag and exact-10% Growth Defrag;
- native EXT, NTFS and exFAT Defrag/Growth Defrag;
- native XFS metadata, relocation and raw-writer safety;
- native Amiga OFS/FFS Defrag/Growth Defrag;
- native HFS+ Defrag, HFSX Growth Defrag, Extents Overflow forks, clean/dirty internal journals and Recover;
- first-party classic-HFS native-C Catalog/Extents Overflow analysis against a generated fragmented raw HFS image;
- GUI worker protocol, transaction handling, mount safety, plugin discovery and package-version ordering;
- canonical `Defragger-${VERSION}.zip` source-archive naming;
- the production prohibition on external filesystem mutation/repair command orchestration.

Pyright is an optional environment gate when the pinned package is available to the test environment; unavailable package retrieval is reported rather than counted as a pass.
