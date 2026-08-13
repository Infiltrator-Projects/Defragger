<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger design

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

Filesystem-neutral C mechanics are implemented once:

- exact interruption-safe `pread` and `pwrite` loops;
- exclusive raw-device opening, geometry and overlap-aware mounted-target rejection;
- rotational/serial-flash policy and resource defaults;
- checked allocation, endian codecs and overflow helpers;
- signal-safe Stop state;
- raw-range and anonymous-memory staging stores;
- generated version ownership.

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

FAT, exFAT, NTFS, EXT2/3/4, XFS, Amiga OFS/FFS and HFS+/HFSX are C-owned mutation engines. Their package-local native sources own format parsing, placement planning, metadata rewriting, staged verification and recovery. Python contains no parallel writer/planner implementation for those filesystems.

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

Revision 83 moves EXT2/3/4 mutation into a native C engine below
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

The writer accepts journaled volumes only when the internal JournalInfoBlock/journal header proves that no transaction needs replay. The journal allocation remains fixed. A non-empty journal is a pre-write failure; journal replay is intentionally outside revision 94.

## Licensing invariant

All first-party Linux Defragger implementation code, GUI glue, build/packaging logic, tests and project documentation use `SPDX-License-Identifier: GPL-3.0-or-later`. The exact comment syntax follows the file format. Non-commentable first-party artefacts use an adjacent `.license` sidecar. `tests/test_spdx_licensing.py` enforces this rule. The repository contains no vendored third-party source; system-provided build/runtime libraries retain their own licence terms.
