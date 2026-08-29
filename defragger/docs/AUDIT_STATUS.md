<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Safety audit status

Status: **complete**

Completed: 2026-08-25

Applies to: release version 1.8.0-140

The production write quarantine has been removed. Analyse, Map, Defragment,
Growth Defrag and Recover are available according to each filesystem plugin's
declared capability. Unsupported or structurally unsafe layouts continue to
fail closed.

## Audited scope

The audit traced every production path from the GUI and command-line operation
boundaries through the native worker, persistent transaction state, raw-device
open, mutation loop, Stop handling, recovery and final read-only verification.

| Write-capable engine | Review and disposable-image evidence |
|---|---|
| FAT12/FAT16/FAT32 | Native mapped relocation journal and replay stages reviewed; FAT12, FAT16 and FAT32 Defragment/Growth Defrag images are reopened and independently checked for payload, directory-reference, contiguity, gap and exact 10% reserve invariants. |
| EXT2/EXT3/EXT4 | Native direct-workspace and verified-stage paths reviewed; exact UUID/type/target identity, workspace restoration, canonical mapping and final reopened-image scans are enforced. |
| NTFS | Native terminal-workspace and verified-stage paths reviewed; serial/target identity, stream digests, mapping pairs, bitmap metadata, recovery replay and final reopened-image scans are enforced. |
| exFAT | Native terminal-workspace manifest and verified-stage fallback reviewed; serial/geometry binding, payload preservation, directory/FAT/bitmap rebuilding, recovery replay and final reopened-image scans are enforced. |
| XFS | Native v5 planner, CRC/metadata update, clean-log gate, payload relocation, durable checkpoint and recovery path reviewed; unsupported feature combinations fail closed. |
| Amiga OFS/FFS | Native full-stage digest, volume token, target identity, commit/recovery and reopened-image payload/layout verification reviewed and exercised. |
| HFS+/HFSX | Native full-stage digest, volume identity, clean-journal gate, commit/recovery and reopened-image payload/layout verification reviewed and exercised. |

Interrupted-operation behaviour was reviewed at each durable journal phase.
Recovery regressions cover preserved transaction artifacts, target mismatch,
corrupt or truncated stages, legacy/unsupported journals, replay from verified
bound stages and cleanup only after successful verification. Stop is honoured
before source mutation or at a filesystem-safe durable boundary; a transaction
that may have reached authoritative writes retains the state required by
Recover.

## Corrective changes made by the audit

1. Mounted-target refusal is now enforced inside every native mutation and
   recovery entry point, including NTFS, exFAT, Amiga OFS/FFS and HFS+/HFSX.
2. Every external recovery stage and SQLite plan is required to be the exact
   filesystem-specific suffix of the selected journal path. A journal cannot
   redirect recovery or cleanup to an unrelated path.
3. Journal temporary files are created exclusively with no-follow semantics;
   pre-existing symbolic links cannot be followed and truncated.
4. Recovery plan databases use SQLite no-follow mode where supported.
5. NTFS now refuses to overwrite an unfinished transaction and requires
   Recover first, matching the other production writers.
6. GUI, CLI and native boundaries retain explicit `--write`, exact target
   confirmation, identity and final verification controls without an
   environment-variable bypass.

## Shared Common dependency

The audited 1.8.0-140 release consumed Infiltratr Common 1.15.0 at exact commit
`d623410f55a071020539fae3f47682896473bd6f`.

On 2026-08-29 the post-audit `main` integration was advanced to Infiltratr
Common 1.15.4 at exact commit
`046406bea2aefa539c74e1038b6c20825eca8af7`. CMake, the gitlink and the local
compiler/installer all verify that same version and commit rather than accepting
an unconstrained checkout.

The consolidation moves generic exact numeric parsing, production endian
decoding, checked geometric allocation growth, atomic recovery-state
publication and durable recovery-state removal into Common. Filesystem record
formats, validation, transaction stages, geometry, relocation policy and the
strict FAT/exFAT destructive CLI quantity grammars remain Defragger-owned.
Recovery paths retain byte-exact persisted path values; only generic mechanics
are shared.

## Release controls and decision

The active protected main ruleset requires the permanent `quality-gate` status
check. Repository administrators and the ChatGPT Codex Connector retain an
explicit direct-main bypass so the repository's main-only workflow remains
usable; that bypass does not authorize publication. The release workflow still
requires a successful push-triggered Project quality gate for the exact current
`main` commit, rechecks `origin/main`, verifies this ruleset through the GitHub
API, rejects an existing tag/release and publishes immutable assets only from
that exact commit.

Shannon Smith gave the explicit release decision for version 1.8.0-140 on
2026-08-25. Publication remains contingent on the exact-head Project quality
gate succeeding for the `Release 1.8.0-140` commit.

Linux Defragger Test Media is outside the production-operation audit. It is a
deliberately destructive filesystem-manufacturing utility with independent
system/boot-disk refusal, canonical-device matching and typed confirmation. It
must only be pointed at media whose complete erasure is acceptable.
