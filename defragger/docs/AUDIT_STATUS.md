<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Safety audit status

Status: **complete**

Completed: 2026-08-25
Extended: 2026-09-12

Applies to: release version 1.8.0-150
Audited source commit: 231c9d9c8e31813deae624da30170bebce44c960
Audited release-governance commit: 3937218c33317772e52716a65684d29b4f08472c

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
| NTFS | Native terminal-workspace and verified-stage fallback reviewed; serial/target identity, stream digests, mapping pairs, bitmap metadata, recovery replay and final reopened-image scans are enforced. |
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
11. The release-critical aggregate quality gate is qualified on a clean GitHub-hosted Ubuntu runner, while the separate heavy/native qualification lane remains on the home `linux-native` runner. Publication semantics and exact-main gating are unchanged.
12. Authoritative raw mutation descriptors are now bound to the OS-level target
    identity and physical capacity recorded by the transaction. EXT and XFS
    commit/recovery paths, NTFS workspace/dirty-state/metadata/stage commits,
    exFAT normal commits and torn-boot recovery, and AFFS/SFS0/HFS+ stage commits
    verify the descriptor that actually receives writes rather than trusting a
    prior pathname check. Regular image replacement between preflight and commit
    therefore fails closed.
13. Both GTK front ends now apply the same MB black/silver application theme.
    Packaging pins the MB Corpo archive to the supplied MBLINK source commit,
    verifies the archive and all three TTF hashes, installs the fonts in the
    Debian package, embeds the verified archive in the local compiler/installer,
    and resolves the installed font's actual family through Fontconfig rather
    than relying on a filename-derived family guess.
14. Test Media now supplies a first-party SFS0 v3 raw fixture instead of the
    stale manual roadmap slot. The fixture contains one 25 MiB file split across
    100 physical extents; its final extent deliberately prevents the required
    10% Growth Defrag reserve. Verification exercises the production SFS parser,
    root/bitmap/B-tree accounting and every deterministic payload block. OFS and
    FFS retain their existing first-party raw creators and payload verification.
15. The allocation-map empty-state Cairo renderer now names the shared MB Corpo
    family instead of falling back to generic Sans. The change is presentation
    only; the complete native/filesystem/GUI test suite and sanitizer lane passed
    before this audit baseline was advanced.
16. The main GTK shell now has a deliberate MB black/silver visual hierarchy
    around the functional allocation map: roomier sections, wrapping legend,
    clearer analysis/mutation/stop controls, improved log and status treatment,
    and a composed MB-styled About dialog. The allocation-map semantics are
    deliberately unchanged: its state colours, 260 px vertical minimum drawing
    area, sampling geometry and allocation data remain intact. Its horizontal
    allocation is fluid so it cannot impose a fixed-width top-level geometry.
    A GUI regression guard fails if those map colours or sizing contracts are
    changed by presentation work. The exact source baseline passed the native/
    filesystem/GUI suite and sanitizer tests before this audit extension was
    advanced.
17. The installed desktop launchers now use absolute `/usr/bin` targets and the
    package refreshes desktop and icon caches after installation. Linux Defragger
    now owns a GUI-first release updater instead of relying on the distribution
    Update Manager to discover GitHub-only releases. The updater selects the
    generic Debian or local native asset to preserve the installed build profile,
    verifies the published SHA-256 manifest plus GitHub asset digests when
    available, and invokes the verified installer through the graphical polkit
    authentication path without a shell. Offline automatic checks fail open to
    the installed application, and a manual Check for Updates desktop action is
    provided. Dedicated updater regressions, the complete native/filesystem/GUI
    suite and the sanitizer lane passed on the exact source baseline before this
    audit extension was advanced.
18. The MB theme received a second presentation-only polish pass for both GTK
    front ends. Disabled primary controls no longer render as bright active
    controls, the operation log styles the actual TextView text surface instead
    of inheriting Mint's grey child surface, summary cards and version badges use
    a quieter graphite/silver hierarchy, operation controls have consistent
    hover/disabled states, and progress/status/legend surfaces are visually
    integrated. The functional allocation map itself remains unchanged: its
    colour contract, dimensions, geometry and data semantics are untouched.
19. The main About surface now consumes the same LINK-style `AboutInfo` contract
    used by the MBLINK/InfiltratorFS family instead of maintaining a Defragger-
    specific hand-built metadata grid. It renders through native `GtkAboutDialog`
    with the shared `link-about-dialog` style hook, 96 px product emblem,
    standardized subtitle/description/authors/website/licence handling and the
    established 560x520 / 520x480 Linux sizing contract. A dedicated regression
    asserts that the runtime window is routed through the LINK-standard view and
    cannot silently fall back to the old custom About implementation. This is a
    presentation-only change; no allocation-map or filesystem mutation code was
    changed, and the tested source baseline passed the complete hosted suite,
    local native/filesystem/GUI tests and sanitizer qualification before this
    audit extension was advanced.
20. Mutation operation logs now record local wall-clock request start and finish
    times plus monotonic end-to-end elapsed time for Defragment, Growth Defrag
    and Recover, including safe-stop and failure completions. The FAT worker
    line-buffers its merged output so configuration, analysis and phase messages
    retain chronological order; it additionally reports setup/analysis,
    read-only preflight/planning, safety-workspace preparation, canonical-layout
    and post-layout-verification durations, total engine elapsed time and
    buffered read/write throughput. Timing uses Infiltratr Common's monotonic
    clock and checked counter-rate primitive and does not alter relocation,
    journal, verification or allocation-map semantics. Deterministic GUI timing
    regressions, FAT12/FAT16/FAT32 integration assertions, the complete hosted
    and local native/filesystem/GUI suites, and ASan/UBSan qualification passed
    on the exact source baseline before this audit extension was advanced.
21. The main GTK window now explicitly publishes a user-resizable contract, and
    the allocation map retains only its vertical drawing minimum instead of a
    horizontal size request. The 1180x820 normal size remains an initial
    preference, while the desktop window manager can expand the shell across the
    monitor when its native maximise control is selected. A regression rejects
    a fixed main-window contract or a restored map width request. No analyser,
    filesystem writer, journal, recovery or allocation-map data semantics were
    changed; the exact source baseline passed the complete hosted and local
    native/filesystem/GUI suites and ASan/UBSan qualification before this audit
    extension was advanced.
22. Live Test Media qualification exposed and closed four independent boundary
    defects. The direct EXT path now retains one exclusive raw-device owner
    while libext2fs performs its internal metadata reopen, releases that owner
    before post-rollback validation, and no longer turns its own lock into an
    `EBUSY` failure. HFS+/HFSX obtains Linux block-device capacity through the
    shared descriptor helper instead of treating block-device `st_size == 0` as
    a tiny volume. Mutation failure dialogs now show one bounded causal line
    while the complete transcript remains in the operation log, preventing an
    oversized modal surface and damaged redraw. XFS proves allocation-tree
    capacity before its lengthy stage permutation, line-buffers chronological
    output, and Test Media pins generated XFS qualification volumes to the
    writer's deterministic CRC-enabled, non-rmapbt feature contract rather than
    inheriting distro-specific mkfs defaults. The exact source baseline passed
    all 33 hosted tests, all 19 local native/filesystem/GUI tests and ASan/UBSan
    qualification before this audit extension was advanced.
23. Live Linux Mint qualification showed that the earlier static resizable
    property check did not prove a usable native maximise path: the frame's
    combined map/log minimum height could exceed the desktop work area, and the
    window manager could remove the maximise control without changing the
    window geometry. The realised GTK window now explicitly publishes the full
    native window-manager function set, while compact 180-pixel map and
    110-pixel log minima leave a genuine resize range; both surfaces still
    expand into surplus height. The request lifecycle also has exactly one
    timing owner: CommandRunner records acceptance, the privileged helper's
    process-start acknowledgement cannot emit a second start event, and the
    presenter defensively treats repeated acknowledgements as idempotent.
    Deterministic regressions assert the native function advertisement,
    flexible geometry and one start timestamp across both transport and
    presentation boundaries. The exact source baseline passed all 33 hosted
    tests, all 19 local native/filesystem/GUI tests and ASan/UBSan qualification
    before this audit extension was advanced. No filesystem analyser, writer,
    journal, recovery or allocation-map data semantics changed.

## Shared Common dependency

The original 1.8.0-140 audit consumed Infiltratr Common 1.15.0 at exact commit
`d623410f55a071020539fae3f47682896473bd6f`.

The current 1.8.0-151 audit extension is bound to Defragger source baseline
`231c9d9c8e31813deae624da30170bebce44c960`. Release qualification rejects any later change beneath the
runtime, native build, Common or packaging trees until the source audit baseline
is explicitly advanced. Release-governance workflows are independently bound to
`3937218c33317772e52716a65684d29b4f08472c`; changes beneath `.github/workflows`
likewise require the governance audit baseline to be advanced. The source baseline
validates Infiltratr Common 1.16.0 at exact commit
`a9db06b11f493c4e6f42bf6c13cc5cc5c73e1fc4`. CMake, the gitlink and the local
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

Version 1.8.0-151 is explicitly authorized for release on 2026-09-12. Any later
version requires a new explicit release decision and a separate `Release <version>`
commit whose exact head passes the Project quality gate.

Linux Defragger Test Media is outside the production-operation audit. It is a
deliberately destructive filesystem-manufacturing utility with independent
system/boot-disk refusal, canonical-device matching and typed confirmation. Its
OFS/FFS/SFS fixtures are regression evidence for the corresponding native
parsers and writers, but Test Media must only be pointed at media whose complete
erasure is acceptable.
