# Changelog

This changelog records user-visible, compatibility, architecture and validation changes for Defragmenter. Detailed commit-by-commit history remains in Git.

## Unreleased

- Fixed a 1.8.0-173 regression where the C++ allocation mapper incorrectly required the generic schema from the FAT12/FAT16/FAT32 worker even though FAT intentionally retains its established cluster-map contract; FAT analysis now has an explicit validated adapter and a real FAT12 end-to-end regression fixture.
- Moved the production allocation mapper, operation dispatcher and privileged helper session into the selective C++17 application-service layer while retaining the established native C filesystem engines.
- Hardened privileged shutdown: the helper now uses `posix_spawn` with a dedicated process group, survives a closed protocol pipe long enough to request cooperative Stop, and waits for the privileged writer to exit.
- Strengthened the C++ mapper's fail-closed schema, identity, geometry and accuracy validation and qualified translated maps against real EXT, NTFS, exFAT, XFS, Amiga OFS/FFS and HFS+ fixtures.
- Corrected exact-bound process-output accounting, hardened mountinfo escape parsing, and declared the C++ runtime dependency in Debian package metadata.
- Extended release auditing so `defragger/native/` is part of the immutable production-source baseline.

- Aligned the documentation roles with Calendar and System Monitor: architecture now owns system contracts, design is concise rationale, validation records evidence boundaries, and Audit Status records only the current safety case.
- Removed duplicated product/manual material from the source-tree README and retained historical audit development in Git/release history instead of the current-state audit.

## Recording policy

Record additions, removals, behavioural fixes, compatibility changes, dependency changes that affect consumers, and material validation/release changes. Pure refactoring needs an entry only when it changes maintenance or portability expectations.

## Historical releases

Existing Git tags and GitHub Releases remain the authoritative identity for exact historical source and release assets. Do not reconstruct detailed historical claims here without evidence from those immutable records.
