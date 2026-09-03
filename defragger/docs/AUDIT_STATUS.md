<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Safety audit status

Status: **complete**

Completed: 2026-08-25
Extended: 2026-09-02

Applies to: release version 1.8.0-142
Audited source commit: 0d563368cea355d8ae251e853a6c83c5e09b68a0
Audited release-governance commit: 34ed63336fd0a9c86b5d4688d6afefb28e19577e

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, sfs, hfsplus

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
| Amiga SFS0 | Native SFS3 root/bitmap/B-tree validation, exact file-extent catalogue, full-stage payload digest, target identity, mounted-target refusal, journal-bound stage path, commit/recovery and reopened-image Defrag/Growth Defrag verification reviewed and exercised. |
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
7. SFS0 is now included in the audited production-writer set and is subject to
   the same mounted-target refusal, journal/stage binding, durable recovery and
   final reopened-image verification requirements as the other native writers.
8. Raw target opens now resolve the canonical path, refuse final-component
   symlinks, compare the opened descriptor identity with the pre-open object and
   repeat mounted-device refusal after the descriptor is acquired. NTFS and
   exFAT apply the same descriptor-identity check in their format-local openers.
9. Release-governance workflows are independently audit-bound. GitHub release
   publication remains conditional on the exact successful Project quality gate,
   while APT publication is a separate retryable workflow.
10. Persistent recovery journals and their derived stages now live under the
    root-controlled `/var/lib/linux-defragger/state/<uid>` namespace. The
    privileged helper accepts exactly one journal directly below the invoking
    user's namespace, and native workers create/validate every parent component
    through directory descriptors using `openat`, `mkdirat`, `O_NOFOLLOW`
    and `fstat`; user-controlled parent symlinks cannot redirect privileged
    recovery files.
11. Release-governance execution was requalified on GitHub-hosted Ubuntu so ordinary qualification no longer depends on the home `linux-native` runner; publication semantics and exact-main gating are unchanged.
12. Authoritative raw mutation descriptors are now bound to the OS-level target
    identity and physical capacity recorded by the transaction. EXT and XFS
    commit/recovery paths, NTFS workspace/dirty-state/metadata/stage commits,
    exFAT normal commits and torn-boot recovery, and AFFS/SFS0/HFS+ stage commits
    verify the descriptor that actually receives writes rather than trusting a
    prior pathname check. Regular image replacement between preflight and commit
    therefore fails closed.

## Shared Common dependency

The original 1.8.0-140 audit consumed Infiltratr Common 1.15.0 at exact commit
`d623410f55a071020539fae3f47682896473bd6f`.

The 1.8.0-141 audit extension is bound to Defragger source baseline
`0d563368cea355d8ae251e853a6c83c5e09b68a0`. Release qualification rejects any later change beneath the
runtime, native build, Common or packaging trees until the source audit baseline
is explicitly advanced. Release-governance workflows are independently bound to
`34ed63336fd0a9c86b5d4688d6afefb28e19577e`; changes beneath `.github/workflows`
likewise require the governance audit baseline to be advanced. The source baseline
validates Infiltratr Common 1.15.4 at exact commit
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

The active protected-main ruleset enforces deletion protection,
non-fast-forward protection and linear history. It intentionally does not require
a branch status check or bypass actor because this repository uses a direct-main
workflow. Publication safety is enforced separately: the release workflow only
runs after a successful push-triggered Project quality gate for the exact current
`main` commit, rechecks `origin/main`, verifies the permanent history rules,
verifies both audit baselines, rejects an existing tag/release and publishes
versioned assets only from that exact commit.

APT publication is deliberately separate from GitHub release creation. A
published release automatically triggers the APT refresh workflow, and the same
exact version/SHA can be supplied to its manual dispatch path if central
publication needs to be retried.

Shannon Smith gave the explicit release decision for version 1.8.0-140 on
2026-08-25. That decision remains historical and does not authorize publication
of 1.8.0-141. Version 1.8.0-141 requires a new explicit release decision and a
separate `Release 1.8.0-141` commit whose exact head passes the Project quality
gate.

Linux Defragger Test Media is outside the production-operation audit. It is a
deliberately destructive filesystem-manufacturing utility with independent
system/boot-disk refusal, canonical-device matching and typed confirmation. It
must only be pointed at media whose complete erasure is acceptable.
