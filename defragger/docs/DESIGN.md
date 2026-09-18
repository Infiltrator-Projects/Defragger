<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Defragmenter design

## Purpose and scope

This document records the architectural rationale and safety contracts of the current implementation. It is intentionally different from a changelog: Git history records when a change happened; this document explains why the present design exists, what assumptions it relies on, and which invariants must remain true when the implementation changes.

The design is for an offline Linux storage tool that can inspect many filesystems and can mutate only those formats for which a first-party, recoverable writer contract is implemented. On-disk metadata is treated as untrusted input and every authoritative write is treated as a safety-critical operation.

## Design objectives and non-goals

The primary objectives are:

1. **Fail closed on uncertainty.** Unsupported features, contradictory geometry, malformed metadata, stale recovery state and ambiguous target identity stop mutation rather than triggering a best-effort guess.
2. **Bind writes to the selected object.** A pathname alone is insufficient; block identity or regular-file identity, capacity and filesystem-specific identity are revalidated across privileged/open/commit boundaries.
3. **Make interruption recoverable.** Once an authoritative source byte may have changed, durable transaction state must be sufficient to complete or safely recover the operation.
4. **Separate planning from authority.** Catalogue and placement state are derived data. They become authoritative only after validation and, where applicable, staged verification.
5. **Verify success independently.** A writer returning without error is not sufficient; the resulting filesystem is reopened and checked against canonical layout and payload invariants.
6. **Keep one implementation of each policy.** Filesystem-neutral mechanics live in the native core or Common; filesystem-format semantics remain with the owning engine.
7. **Bound hostile inputs and resource use.** Integer arithmetic, vector growth, tree traversal and selected planning algorithms have explicit overflow or resource limits.

The project is not a general-purpose filesystem repair suite, an online defragmenter, a replacement kernel filesystem driver, or a promise to support every valid historical feature combination. It does not attempt automatic journal replay for formats whose writer contract does not explicitly implement it. Unsupported cases are expected to be rejected.

## Assumptions and trust boundaries

The implementation assumes that the Linux kernel, libc, required system libraries and storage hardware honour their documented interfaces. Filesystem bytes, recovery artefacts outside the trusted state namespace, GUI input and user-selected paths are not trusted merely because they were previously validated.

A privileged-process or root compromise is outside the threat model. Hardware that acknowledges durable writes but later loses or silently corrupts them is also outside what software alone can prove. `fsync()`/durable-publication contracts are therefore used as the strongest software-visible persistence boundary, not as a claim that storage firmware is infallible.

Regular image files may be replaced between preflight and open, and block devices may change mount relationships. The native target-opening sequence therefore validates object identity and mounted overlap on both sides of the open boundary. Filesystem writers then bind the descriptor to format-specific identity such as UUID, serial number and geometry where that format provides it.

## Safety invariants

The following invariants are design requirements, not implementation hints:

- **I1 - target identity:** authoritative writes are issued only to a descriptor that still matches the transaction's selected target and expected capacity.
- **I2 - mounted-overlap exclusion:** raw mutation refuses a mounted target and any block-topology mapping whose address space overlaps the selected target.
- **I3 - validated plan:** no filesystem metadata is rewritten from an unvalidated catalogue or placement plan.
- **I4 - durable recovery boundary:** before an operation enters a phase from which source bytes may require recovery, the state needed by Recover is durably published.
- **I5 - monotonic transaction state:** recovery phases cannot silently move backwards or replace an existing unfinished transaction with unrelated state.
- **I6 - safe Stop:** Stop is cooperative and is observed only at boundaries where on-disk state is unchanged, valid or recoverable.
- **I7 - independent completion check:** mutation success requires a reopened read-only verification of payload/layout invariants.
- **I8 - unsupported means no write:** unknown features, impossible geometry and failed integrity checks reject mutation before the affected structure is trusted.

These invariants are mapped to executable evidence in [`VALIDATION.md`](VALIDATION.md) and to the release-specific safety case in [`AUDIT_STATUS.md`](AUDIT_STATUS.md).

## Engineering trade-offs

Direct userspace mutation increases implementation complexity compared with asking a mounted kernel driver to relocate files. It is retained because the project requires deterministic physical placement and a uniform recovery model that can be independently inspected. The consequence is a deliberately narrower write-support matrix and a larger verification burden.

Persistent staging consumes temporary storage and I/O, but it gives recovery a stable source of truth across process interruption. Engines use smaller terminal workspaces only where the filesystem-specific dependency graph makes that safe; otherwise a verified stage is preferred over reducing temporary-space cost.

The exact 10% Growth Defrag reserve is intentionally stricter than a heuristic 'leave some room' policy. It makes the postcondition deterministic and testable at the cost of rejecting layouts that cannot satisfy the exact reserve.

Generic parsing, arithmetic, escaping and I/O mechanics are delegated to the pinned Common library only when their semantics match. Device safety, filesystem geometry, placement and transaction policy stay local even when a generic abstraction might superficially reduce code, because moving policy into Common would weaken the ownership boundary.

## Complexity and resource bounds

The project does not claim one complexity bound for every filesystem format, but it makes resource behaviour explicit where hostile metadata could cause unbounded work:

- catalogue scans are bounded by filesystem geometry and by format-specific limits on traversed blocks, inodes or extents;
- range/vector normalisation is generally linear to collect plus O(n log n) where sorting is required;
- allocation bitmaps use storage proportional to the represented allocation domain and are rejected when size arithmetic cannot be represented safely;
- the NTFS low-layout subset planner is pseudo-polynomial in its bounded target and is capped by a fixed 256 MiB planning-memory limit rather than allowing unbounded allocation;
- XFS and Btrfs metadata walks apply explicit defensive traversal ceilings so corrupt cyclic/adversarial trees terminate with failure.

These are engineering bounds, not a formal proof of worst-case execution time. Performance changes must preserve the safety invariants above.

## Canonical layouts

Defragment succeeds only when a final read-only rescan confirms that supported regular files and directories are contiguous, all movable allocations occupy the earliest legal data units, and no free data unit exists below the final allocated object.

Growth Defrag applies the same rules while leaving exactly 10% of each regular file's allocated length free immediately after that file. Those intentional reserves are the normal permitted free gaps inside the movable layout.

Each filesystem defines its first legal data unit. Mandatory boot, allocation, journal and fixed metadata regions are not counted as movable data gaps. If fixed metadata divides the legal data space, a residual free suffix immediately before that metadata is also permitted only when it is smaller than every complete legal movable object span (including the exact Growth Defrag reserve). Such boundary slack is therefore impossible to consume without fragmenting an object or violating its reserve; larger potentially avoidable gaps remain a planning failure.

## Layer responsibilities

### UI layer — `gui/ui/`

`MainWindow` is the composition root and binds user intent to services. It does not
own subprocesses, privilege-helper state, widget construction, volume
collection policy, operation validation, worker-event parsing or transient
operation progress state.

- `WindowView` owns GTK widgets, dialogs and presentation updates.
- `OperationCoordinator` owns the complete analysis and mutation lifecycle,
  permission selection, command continuations and post-operation map refresh.
- `VolumeCoordinator` owns discovery, image validation, per-window selection,
  allocation-map cache and journal naming; `VolumeStore` is its collection
  state.
- `operation_planner` validates mutations and constructs standard commands.
- `CommandRunner` owns ordinary processes and the one-command lifecycle.
- `PrivilegeSession` exclusively owns `pkexec`, helper JSON IPC, the privileged
  allowlist and privileged safe Stop.
- `OperationPresenter` owns progress, Stop, delayed-close, typed completion,
  live-event application and redraw timing through a narrow view protocol.
- `LiveEventController` converts typed worker events into view-model updates.
- `BackendCatalog` owns an immutable per-window plugin manifest.
- `MapPresentation` validates analyser data before widget or cache mutation.

The runner and policy modules do not import GTK. The view does not import
subprocess, device discovery, filesystem backends or operation policy.

### Python orchestration — `gui/engine/`

The Python orchestration layer resolves workers and filters common command-line
options. Plugin declarations are validated by `gui/backends/`; the
filesystem-neutral event protocol is in `gui/core/protocol.py` so workers and
the UI share it without importing each other's orchestration packages.

### Shared native core — `src/core/`

Filesystem-neutral Defragmenter mechanics are implemented once:

- adapters over Common's exact positioned I/O with Defragmenter's error policy;
- exclusive raw-device opening, geometry and overlap-aware mounted-target rejection;
- target identity/capacity binding across authoritative write boundaries;
- rotational/serial-flash policy and resource defaults;
- signal-safe cooperative Stop state;
- machine-readable result emission using Common JSON escaping;
- generated version ownership.

Generic C primitives that are also useful to other Infiltrator applications are
not reimplemented here. Linux Defragger pins Infiltratr Common 1.19.2 at
exact commit `44409af17c89b6ece6b4bcb2c0c133213c695c23` and links the canonical
`InfiltratrCommon::Common` CMake target. Common owns strict integer parsing and
range validation, bounded strings, line-end trimming, checked and saturating
arithmetic, checked geometric growth, endian byte access, percentage
calculation, exact sequential/positioned I/O, bounded realpath handling, JSON
escaping, small sysfs scalar reads and lexical prefix/path helpers. Device
safety, raw-storage policy, Stop state and each filesystem's journalled staging
and transaction mechanics remain local because those semantics are
application-specific.

This directory contains no filesystem registration or dispatch logic.

### Filesystem packages — `gui/filesystems/<format>/`

This is the single authoritative filesystem hierarchy. Every filesystem owns
its format-specific probe, analyser, planner, writer and verifier beneath its
GUI package. `gui/backends/registry.py` is the only filesystem registry.

A plugin may own a private native C engine beneath the same package. C is the
preferred implementation language for filesystem parsing and mutation; Python
is retained only where it provides GUI/backend glue or where a filesystem has
not yet completed its C migration. FAT keeps its C implementation in
`gui/filesystems/fat/native/`; XFS keeps its complete raw engine in
`gui/filesystems/xfs/native/`; classic HFS keeps its first-party direct read-only analyser in
`gui/filesystems/hfs/native/`. Those binaries are workers of the GUI plugin,
not independently registered filesystem plugins. There is deliberately no
`src/filesystems/` tree and no native filesystem ABI.

FAT, exFAT, NTFS, EXT2/3/4, XFS, Amiga OFS/FFS, Amiga SFS0 and HFS+/HFSX are C-owned mutation engines. Their package-local native sources own format parsing, placement planning, metadata rewriting, staged verification and recovery. Python contains no parallel writer/planner implementation for those filesystems.

XFS is C-owned end to end. `xfs_catalog.c` owns allocation-group, free-space,
inode-btree and bmbt decoding; `xfs_plan.c` owns canonical placement, payload
checksums/permutation and direct inode extent rewriting; `xfs_metadata.c` owns
`bnobt`/`cntbt`/`rmapbt` rebuild, AGFL reserve management, v5 CRCs and raw
verification; `xfs_worker.c` owns staging, durable journal/commit and recovery.
The Python XFS plugin is only a GUI adapter. No XFS filesystem driver or
xfsprogs mutation utility participates in the production path.

### Shared transaction contract

`gui/core/transaction.py` owns journal schema validation, legal phase
transitions, atomic replacement and durable deletion. Filesystem transaction
modules provide the format-specific payload and recovery actions. A worker may
not overwrite an existing recovery record with an invalid or regressive phase.
Tests can inject a failure immediately before or after every durable phase
write through an explicit in-process checkpoint object; production workers do
not expose an environment or command-line fault switch.

## Operation lifecycle

A write operation follows one standard lifecycle:

1. Identify the plugin and validate the declared operation.
2. Reject a mounted block target and validate exact write confirmation.
3. Open the raw target and establish Stop and journal state.
4. Scan the complete source model and reject unsupported states before writing.
5. Build and validate the canonical target plan.
6. Stage either the overlapping data or a complete sparse working image using
   the format's recoverable persistent transaction.
7. Persist transaction state and perform bounded durable writes.
8. Publish live events only for completed durable changes.
9. Honour Stop at the next safe boundary.
10. Rescan and verify the canonical invariant before success.

Filesystem plugins own metadata transaction details. The native core owns only the mechanics that are identical across formats.

An enabled writer must have a persistent recovery path for every authoritative
write. Anonymous memory is not a valid sole recovery source after the first
authoritative filesystem change.

## Direct writer rule

A write-capable plugin opens the unmounted target directly. It may link system-provided
userspace libraries in-process, but Linux Defragger does not vendor third-party source.
A writer may not mount the target, call a mounted-filesystem relocation interface,
or launch an external filesystem mutation/repair program. The plugin owns allocation planning, metadata updates,
checksums, staging, verification, commit and recovery. A production architecture
test rejects known external filesystem mutation/repair utilities and loop/mount
orchestration if they reappear in production code or package dependencies.

Normal raw I/O system calls still pass through Linux; the filesystem driver does not choose placement.

## EXT native staged transaction

The EXT2/3/4 mutation path is a native C engine below
`gui/filesystems/ext4/native/`. The worker links `libext2fs` directly in-process
and performs its own catalogue, deterministic placement, raw block permutation,
inode/block-map updates, staged verification, durable commit and recovery. No
external filesystem command is part of the production EXT path.

The clean source is cloned to a persistent sparse working image and remains
unchanged while relocatable inode/data/mapping blocks are placed canonically.
Fixed group metadata stays fixed. The private image is verified internally,
including allocation state and payload identity, before commit begins. Commit derives write ranges from the verified final allocation state and writes only allocated blocks, with a durable physical cursor so Recover can resume idempotently after interruption. Growth Defrag verifies the exact 10% reserve policy before
source commit.


## Classic HFS direct analyser

`gui/filesystems/hfs/native/analyser.c` is a first-party read-only implementation. It parses the classic HFS Master Directory Block, Extents Overflow B-tree and Catalog B-tree directly from raw storage, follows data/resource-fork extent chains, and reports exact file fragmentation without mounting HFS or invoking an external HFS utility. The former bundled hfsutils/libhfs source is absent from the repository and package. Unsupported structural cases fail closed.

## Amiga OFS/FFS

`gui/filesystems/affs/native/` is the authoritative first-party OFS/FFS implementation. It performs raw DOS\0..DOS\7 identification, allocation bitmap decoding, directory/hash-chain traversal, file/list-block cataloguing, relocation planning, OFS/FFS data rewriting, staged verification, allocated-range commit and recovery. The previous bundled Python `amitools` runtime is not part of production or packaging.


## HFS+/HFSX native staged transaction

`gui/filesystems/hfsplus/native/` owns HFS+ and HFSX identification, allocation-file decoding, Catalog and Extents Overflow B-tree traversal, fork placement, extent-descriptor updates, payload hashing, live-map emission, staged verification, allocated-range commit and Recover. Filesystem B-tree topology stays fixed. Existing overflow-record keys and extent counts are preserved while physical extent starts are rewritten to make the fork contiguous.

The writer accepts journaled volumes only when the internal JournalInfoBlock/journal header proves that no transaction needs replay. The journal allocation remains fixed. A non-empty journal is a pre-write failure; journal replay is intentionally outside the supported HFS+/HFSX writer contract.

## Filesystem plugin and worker contract

Each discoverable filesystem package under `gui/filesystems/<id>/` exposes `BACKEND`
from `plugin.py`; `gui/backends/registry.py` is the sole filesystem registry and
dispatch authority. Read-only plugins provide probe/map behaviour. Write-capable
plugins additionally declare their supported operations and private native worker.
Filesystem-specific parsing, planning, writing and verification stay below the
owning filesystem package; filesystem-neutral mechanics stay in `src/core/`.

Write workers use the standard invocation shape
`WORKER OPERATION TARGET --write --confirm TARGET --journal PATH [options]`. Common
options include live-map sizing, the fixed 10% Growth Defrag reserve, RAM staging,
worker count and transaction sizing where supported. Workers publish typed `@@PHASE`,
`@@LIVE_RANGES` and `@@RESULT` records. Human-readable log text is not an API, and a
successful mutation emits exactly one semantic result for `defrag`, `growth-defrag`
or `recover` with status `completed`, `not-needed` or `stopped`.

Every first-party plugin, native worker and associated test uses the project
`GPL-3.0-or-later` SPDX identifier. Architecture and licensing tests enforce these
invariants.

## Licensing invariant

All first-party Linux Defragger implementation code, GUI glue, build/packaging logic, tests and project documentation use `SPDX-License-Identifier: GPL-3.0-or-later`. The exact comment syntax follows the file format. Non-commentable first-party artefacts use an adjacent `.license` sidecar. `tests/test_spdx_licensing.py` enforces this rule. The repository contains no vendored third-party source; system-provided build/runtime libraries retain their own licence terms.

## Validation and evidence

The design is exercised through warnings-as-errors builds, native/unit tests, disposable filesystem-image tests, independent post-operation verification, fault-injected transaction tests, architecture/safety regressions, ASan/UBSan, packaging tests and exact-head release gating. The methodology, evidence independence and known validation limits are documented in [`VALIDATION.md`](VALIDATION.md).

The release-specific audit is deliberately separate from this design document. [`AUDIT_STATUS.md`](AUDIT_STATUS.md) binds the enabled writer set and current release to exact audited source/governance commits.

## Technical references

Filesystem and platform references used to interpret on-disk structures and system-call durability/identity semantics are catalogued in [`REFERENCES.md`](REFERENCES.md). Implementation comments should cite a reference when a non-obvious algorithm follows a published on-disk rule; comments should otherwise explain project-specific invariants and rationale rather than restate the code.
