# Safety audit status

Status: **complete**

Completed: 2026-08-25  
Extended: 2026-09-19

Applies to: release version 1.8.0-172
Audited source commit: e31601822d264f0cd977b6763b3d89f01a6175b2
Audited release-governance commit: 265a3dc8d6ceddd83f6c0e0cb4f38aea61254f94

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, sfs, hfsplus

This document records the **current** write-safety case. Historical audit-development detail remains available in Git history and immutable release tags rather than being repeated here.

## Current safety case

The enabled writers share the following release requirements:

| Risk | Required contract | Principal evidence | Remaining limit |
| --- | --- | --- | --- |
| Wrong target or mounted overlap | exact target confirmation, descriptor identity/capacity checks and mounted-overlap refusal | safety/native tests plus filesystem worker checks | a compromised privileged OS is outside the model |
| Unsupported or corrupt metadata | complete pre-write validation and fail-closed feature checks | negative fixtures and filesystem-native tests | untested feature combinations remain unsupported |
| Interrupted mutation | durable filesystem-specific transaction state before recovery can be required | transaction/fault-injection and Recover suites | hardware that lies about persistence is outside the model |
| Unsafe Stop | cooperative Stop only at unchanged, valid or recoverable boundaries | worker/transaction Stop regressions | Stop is not an arbitrary mid-write abort |
| Silent payload/layout damage | reopened read-only verification before success | disposable-image verification and payload/layout checks | shared parser assumptions can still create common-mode risk |
| Publication from unaudited source | exact source/governance baselines plus exact-head release gates | `tests/test_release_gate.py` and GitHub workflows | protects project publication, not downstream repackaging |
| Destructive Test Media misuse | separate utility, system-disk refusal and repeated confirmation | Test Media safety tests | an explicitly selected sacrificial disk can still be erased |

The validation method and its limits are defined in [VALIDATION.md](VALIDATION.md).

## Audited write scope

The completed audit covers these first-party write/recovery engines:

- **FAT12/FAT16/FAT32** — native direct analysis, canonical relocation, exact Growth Defrag reserve and Recover.
- **exFAT** — native catalogue/relayout, exact Growth Defrag reserve and Recover.
- **NTFS** — native fail-closed preflight, canonical supported-subset relocation, exact Growth Defrag reserve and Recover.
- **EXT2/EXT3/EXT4** — native staged transaction using the linked libext2fs API in-process, followed by verification and Recover.
- **XFS** — native raw userspace catalogue, planning, metadata rewrite, verification and Recover for the explicitly supported v5 contract.
- **Amiga OFS/FFS** — native raw catalogue, relocation, verification and Recover.
- **Amiga SFS0** — first-party native supported-subset relayout and Recover.
- **HFS+/HFSX** — native staged transaction and Recover for supported clean-journal states.

Read-only support for other formats is not promoted to write support by this audit. Unsupported or ambiguous layouts continue to fail closed.

## Safety invariants

The audited writers are expected to preserve these invariants:

1. writes remain bound to the selected target and expected capacity;
2. mounted or overlapping raw targets are refused;
3. no metadata rewrite proceeds from an unvalidated catalogue or plan;
4. recovery material is durable before source bytes can require recovery;
5. transaction state cannot silently regress or switch targets;
6. Stop is observed only at a valid/recoverable boundary;
7. success requires reopened verification of payload and layout;
8. unsupported or structurally unsafe states do not write.

Architecture ownership is defined in [ARCHITECTURE.md](ARCHITECTURE.md); design rationale is in [DESIGN.md](DESIGN.md).

## Dependency baseline

The audited production tree consumes Infiltratr Common 1.19.2 at exact commit `44409af17c89b6ece6b4bcb2c0c133213c695c23`. CMake, the native installer and release regressions verify the same pin.

Common owns reusable mechanisms such as checked arithmetic, strict parsing, endian access, bounded growth, path/string helpers, JSON escaping and exact I/O. Defragmenter retains filesystem structure, target-safety, placement, transaction, recovery and user-facing failure policy.

## Release controls and decision

The repository uses a `direct-main` development model. The active main ruleset provides the permanent history protections checked by release automation; publication itself is guarded by the exact-head **Project quality gate**, exact audit baselines and immutable tag/release checks.

The release workflow verifies the `protected-main` history rules, the current `main` SHA, the audited production baseline and the audited release-governance baseline before publication. The governance baseline includes the canonical repository-document paths used by release verification. A release still requires an **explicit release decision** represented by a `Release <version>` commit whose exact head passes the gate.

APT publication is a separate retryable workflow bound to the published release version and SHA.

Version 1.8.0-172 is the current audited release line. The baseline also includes the packaging correction that installs only documentation contained in the self-contained application source tree. Any later change beneath audited production/build/package paths requires a new source audit baseline before release.

## Historical record

Earlier audit extensions documented individual implementation fixes, branding changes, Common migrations and release-pipeline corrections inline. Those details are preserved in Git history and release tags. Keeping them out of this current-state safety case prevents historical narrative from becoming a second changelog.
