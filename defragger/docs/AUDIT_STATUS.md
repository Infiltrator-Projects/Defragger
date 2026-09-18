<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Safety audit status

Status: **complete**

Completed: 2026-08-25
Extended: 2026-09-18

Applies to: release version 1.8.0-170
Audited source commit: 7b041922a1d0097e1c29f67dc1554ff209f19bf0
Audited release-governance commit: 0c72737a9a16f1fc46ed66f1ccb466ea3a45e5f2

Audited writer IDs: fat12, fat16, fat32, exfat, ntfs, ext4, xfs, affs, sfs, hfsplus

The production write quarantine has been removed. Analyse, Map, Defragment,
Growth Defrag and Recover are available according to each filesystem plugin's
declared capability. Unsupported or structurally unsafe layouts continue to
fail closed.

## Safety argument summary

The audit is structured as a safety case: each material hazard has a control, an enforcement point and executable evidence. The chronological change record below is supporting history, not the primary argument for safety.

| Hazard | Principal control | Enforcement / evidence | Residual limitation |
| --- | --- | --- | --- |
| Wrong target or pathname replacement | canonical open plus descriptor identity/capacity and filesystem identity binding | `src/core/ld_device.*`, native target checks, `tests/test_safety.py` | malicious root or a compromised kernel is outside the threat model |
| Mounted or overlapping storage mapping | parent/child/holder/slave overlap closure with pre/post-open refusal | native core, architecture/safety tests | depends on Linux exposing the relevant topology in the current namespace |
| Malformed or unsupported on-disk state | strict parser validation and fail-closed feature gates | native negative fixtures and filesystem suites | unsupported valid feature combinations remain intentionally unavailable |
| Integer/resource exhaustion from hostile metadata | checked arithmetic, bounded vectors and explicit traversal/planning ceilings | Common arithmetic, native tests, ASan/UBSan | ceilings may reject unusually large but otherwise valid filesystems |
| Interruption before authoritative writes | verified staging plus durable phase publication | transaction/fault-injection tests | storage must honour software-visible durability semantics |
| Interruption after authoritative writes | recoverable journal/workspace state retained until final verification | filesystem recovery suites | physical media failure can exceed recoverable software state |
| Corrupt or stale recovery artefacts | target/stage binding, schema/phase validation and digest/identity checks | transaction/native recovery regressions | deliberate privileged tampering is outside the model |
| False success after mutation | reopened read-only payload/layout verification | disposable-image integration tests and independent verifier scripts | testing is empirical, not a formal proof |
| Publication from unaudited source | exact source/governance baselines plus exact-head quality/release gates | `tests/test_release_gate.py`, GitHub release workflow | protects project publication, not downstream repackaging |
| Destructive Test Media misuse | separate utility, system-disk refusal, repeated privileged confirmation | Test Media safety tests | operator can still destroy explicitly selected sacrificial media |

The verification methodology and limits of this evidence are documented in [VALIDATION.md](VALIDATION.md).

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

## Audit history and corrective changes

The entries below preserve traceability for material corrections and qualification
work. They are supporting history; the current safety obligations are the
hazard/control matrix above and the invariants in [DESIGN.md](DESIGN.md).

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
17. Defragmenter is again strictly package-manager updated. The application
    launcher now starts the installed GTK program directly, the private GitHub
    release downloader/pkexec installer and manual Check for Updates desktop
    action have been removed, and the Debian package remains the sole installed
    update unit. New releases are published into the configured Infiltrator APT
    repository and are discovered and installed by the normal distribution
    Update Manager. A regression test now fails if an application-owned updater
    is reintroduced into the packaged launcher, desktop entry or install set.
18. The MB theme received a second presentation-only polish pass for both GTK
    front ends. Disabled primary controls no longer render as bright active
    controls, the operation log styles the actual TextView text surface instead
    of inheriting Mint's grey child surface, summary cards and version badges use
    a quieter graphite/silver hierarchy, operation controls have consistent
    hover/disabled states, and progress/status/legend surfaces are visually
    integrated. The functional allocation map itself remains unchanged: its
    colour contract, dimensions, geometry and data semantics are untouched.
19. The main About surface now consumes the same LINK-style `AboutInfo` contract
    used by the MBLINK/InfiltratorFS family instead of maintaining a Defragmenter-specific hand-built metadata grid. It renders through native `GtkAboutDialog`
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
24. Minix allocation-map accounting now locates the cell containing each zone
    with a bounded binary search over the ordered map cells. The previous
    linear lookup made a full-resolution 1 GiB Minix map quadratic: roughly
    262,144 zones multiplied by 262,144 cells. A sparse 1 GiB regression now
    exercises one map cell per zone and verifies exact used/free accounting;
    the native warnings-as-errors and ASan/UBSan tests pass. The change is
    read-only and does not alter Minix filesystem parsing or allocation data.
25. APT refresh now follows the successful Build and publish release workflow
    through a bounded `workflow_run` chain. Releases created with GitHub's
    workflow token do not emit another workflow-triggering release event, so
    the previous listener could leave a valid GitHub release unpublished in
    Update Manager. The refresh resolves exactly one immutable published
    release for the completed workflow SHA before dispatching and verifying
    the central catalogue. Manual retry remains available with an exact version
    and release SHA. The release-contract regression enforces this trigger and
    identity binding.
26. The GUI updater now passes the manifest-verified asset digest to a
    root-owned updater helper. The helper opens the user download with
    no-follow semantics, verifies ownership and permissions, copies and hashes
    the bytes into a root-only staging file, and invokes only the fixed APT or
    native installer path from that staged copy. A pathname replacement after
    GUI verification therefore cannot redirect a privileged installation.
27. The home Linux native/heavy qualification lane is independent of the
    hosted release gate and is now manual-only. It remains available on the
    `linux-native` runner for deliberate workstation/hardware qualification,
    with explicit command, pkg-config, Python-module or executable diagnostics
    when that machine is available. Ordinary pushes and releases no longer
    create queued work on an offline home runner.
28. The shared dependency was advanced to immutable Infiltratr Common 1.18.0 at
    commit `0f458a7145f3a996ff19f11bf9d0893b91ae5cbb`. The exact migrated
    production tree at `81f51300d3369b28f572f4777b65aa30bed9d308` built
    warnings-as-errors, passed all 33 hosted native/filesystem/GUI tests and
    passed the hosted ASan/UBSan lane. Its only initial quality-gate failure was
    the intentional stale-audit-baseline guard, which that extension advanced.
29. The shared dependency is advanced again to immutable Infiltratr Common 1.18.1
    at commit `dcfa6fee9e9263a0dce5c137054d4adf130c2f25`. Common 1.18.1 is a source-licence-hygiene patch;
    its runtime behaviour and public ABI are unchanged from 1.18.0. The exact
    Defragmenter migration tree at `ddca104d5464c56ed44aef699e1a4d883e17e7a3` built warnings-as-errors,
    passed all 33 hosted native/filesystem/GUI tests, and passed the hosted
    ASan/UBSan lane. Its only initial quality-gate failure was the intentional
    stale-audit-baseline guard, which this extension advances.
30. The Linux desktop window contract is corrected in exact production tree
    `3b7d0b37283c8b493ca7fbb11c969861c83a9377`: startup geometry is reduced and capped to the monitor
    work area, the GTK window explicitly advertises resize/move/minimise/
    maximise/close functions, and the body has a scroll fallback for smaller
    or RDP work areas. That exact tree built warnings-as-errors, passed all 33
    hosted native/filesystem/GUI tests, and passed the hosted ASan/UBSan lane.
    Its only initial quality-gate failure was the intentional stale-audit-
    baseline guard, which this extension advances.
31. The main Defragmenter GUI now has an explicit three-state appearance
    contract: Follow system, Day and Night. Follow system deliberately leaves
    GTK/Mint colours authoritative while preserving Infiltrator typography;
    Day and Night apply scoped light and graphite/silver palettes. The selected
    mode is persisted per user and synchronised across every open Defragmenter
    window. This is a presentation-only change: filesystem engines, allocation
    maps, operation planning, raw-device safety, journals and recovery semantics
    are unchanged. Exact production tree
    `80194442f9234360f0b5e9eaaec61021a2c2bb5a` passed all 33 hosted
    native/filesystem/GUI tests and the hosted ASan/UBSan lane. Its only
    quality-gate failure was the intentional stale-audit-baseline guard, which
    this extension advances.
32. Theme palette ownership is now centralised in Infiltratr Common 1.19.2
    at exact commit `44409af17c89b6ece6b4bcb2c0c133213c695c23`.
    Defragmenter's Python GTK layer retains only toolkit selectors and preference
    mechanics; its generated Day/Night token module is regression-checked
    directly against Common's canonical design JSON. No filesystem-specific
    analyser, planner, writer, journal, recovery or allocation-map semantics
    changed. Exact production tree
    `fa1b5537292e27fa0f6bd519f6fa3d0921496223` built warnings-as-errors,
    passed all 33 hosted native/filesystem/GUI tests, and passed the hosted
    ASan/UBSan lane. The only quality-gate failure was the expected stale-audit
    guard before this baseline advance.
33. Common 1.19.2 now owns more of Defragmenter's generic mechanics without
    absorbing filesystem policy. FAT relocation vectors and EXT range/block
    vectors use Common's checked geometric allocator; FAT journal basename
    handling uses Common's POSIX lexical helper; EXT, NTFS, exFAT and XFS
    mutation-option numbers use Common's strict whole-string parser instead of
    `atoi()`; and FAT, Minix, SFS and UFS size calculations use Common checked
    allocation arithmetic. Filesystem geometry, placement, transaction,
    recovery and error-policy decisions remain Defragmenter-owned. Exact source
    tree `0169036334b90312462b72ec65794a176134e3c6` built all first-party C with warnings as errors,
    passed the complete 33-test native/filesystem/GUI/release suite, and passed
    the hosted ASan/UBSan lane. Its remaining quality-gate failure was solely
    the expected stale-audit-baseline invariant that this extension advances.
34. The final Common 1.19.2 consolidation keeps Defragmenter policy local while
    removing the remaining generic duplication. EXT planner vectors now use
    Common checked geometric growth; all write workers share one Defragmenter
    @@RESULT adapter backed by Common JSON escaping; Btrfs, NTFS, exFAT and XFS
    use Common percentage calculation; NTFS, swap, UFS, XFS and core path
    arithmetic use Common checked operations; superseded local realloc and
    atomic-temp/fsync helpers are removed; and Test Media now consumes Common
    parsing, trimming, basename/realpath, endian, exact-I/O and array-growth
    primitives. Filesystem geometry, relocation, transaction, recovery and
    destructive-target policy remain Defragmenter-owned. Exact source tree
    `b999eb48a57456f34e3ce5ecb366bffc287b0276` built all first-party C with warnings as errors,
    passed all 33 native/filesystem/GUI/release tests, and passed the hosted
    ASan/UBSan lane. Its only remaining quality-gate failure was the intentional
    stale source-audit baseline that this extension advances. The quality-gate
    concurrency key was also renewed after an orphaned self-hosted run retained
    the previous key; runner selection, checks and cancel-in-progress semantics
    are unchanged, and release governance is now bound to
    `29ea8bc75a51bb8779d5507bee866152802f678f`.

35. A postgraduate engineering-documentation pass makes the existing safety
    argument explicit without changing filesystem algorithms. Native core and
    filesystem headers now document ownership, units, target-identity and Stop
    contracts; NTFS/exFAT comments explain non-obvious on-disk codec rules.
    `DESIGN.md` now records objectives, non-goals, trust boundaries, eight
    safety invariants, trade-offs and resource bounds. `VALIDATION.md` adds
    claim-to-evidence traceability and residual-risk limits, while
    `REFERENCES.md` records authoritative/corroborating technical sources.
    During qualification, two stale 1.8.0-164 rename assertions and a lost
    executable bit on `packaging/build-deb.sh` were also corrected. Exact
    production/package baseline `49e20783f3c74177d18f9c858ef27e74938340e9` and qualification head
    `21620048f6215413924fe0175ef3e38007b16d7b` built with warnings as errors; the complete 33-test
    native/filesystem/GUI/release suite and Hosted ASan/UBSan lane passed.
    The only remaining gate failure was the intentional stale-audit-baseline
    assertion that this extension advances.

36. The product-branding completion removes the retired pre-1.8.0-166
    product labels from user-facing source, GUI messages, plugin warnings, engineering
    documentation and release metadata. Public source archives are now named
    `Defragmenter-<version>.zip`. Stable compatibility identifiers remain
    unchanged where renaming would break upgrades, persisted state or external
    automation: the Debian package and executable family remain
    `linux-defragger`, the desktop application ID remains
    `io.github.linuxdefragger`, and existing state paths, environment
    variables and journal magic retain their established identifiers. A
    permanent architecture regression rejects the retired product labels from
    selected user-facing surfaces and verifies the Defragmenter source-archive
    contract. Exact audited source baseline `f4c538e4d8bfa724d8de53b391d76325e2f993d3` contains the completed rename;
    release-governance is independently bound below; publication remains
    conditional on the exact-head quality gate and immutable release checks.

37. The quality-gate concurrency identity was renewed from v2 to v3 after
    GitHub left a cancelled self-hosted job attached to
    `Latitude-5550-Linux` with no executed steps, preventing the current
    release gate from starting. Runner selection, build/test commands,
    sanitizer qualification, release invariants and cancel-in-progress
    semantics are unchanged. The governance-only change is bound to exact
    commit `24efb9f41a449148b49547cb7db9d1c6f24f1403`; the audited product source remains
    `f4c538e4d8bfa724d8de53b391d76325e2f993d3`.

38. Qualification of the completed branding pass exposed one accidental
    compatibility break: the legacy Debian build-profile metadata header had
    been changed as though it were display branding. The package builder and local compiler now retain that established
    metadata key while all user-visible product text remains Defragmenter.
    This correction changes no filesystem algorithm, package name, executable,
    application ID, state path or journal format. The corrected audited source
    baseline is `f4c538e4d8bfa724d8de53b391d76325e2f993d3`.

39. A second orphaned self-hosted cancellation required one further
    concurrency-identity renewal, from v3 to v4. Exact governance commit
    `5420b0d12049ae9b9f6489fc0a33d2676ca488db` changes only the quality-gate concurrency group; runner
    selection, build and test commands, sanitizer qualification, release
    invariants and cancel-in-progress semantics are unchanged. The audited
    product source baseline remains
    `f4c538e4d8bfa724d8de53b391d76325e2f993d3`.

40. The visible-branding follow-up completes the Defragmenter rename without
    changing compatibility identities. GitHub release deliverables now use
    `Defragmenter-<version>-amd64.deb`,
    `Defragmenter-<version>-local-folder.run` and
    `Defragmenter-<version>.zip`; the About/project link targets the renamed
    `Infiltrator-Projects/Defragmenter` repository; and Defragmenter Test Media
    no longer displays the retired pre-1.8.0-167 product name and creates the
    branded `Defragmenter-TestData` directory. The Debian package identity,
    installed executable family, application ID, state paths, environment
    variables and journal formats remain unchanged for upgrade and persisted
    state compatibility. Exact source baseline
    `9299cfe323b55281a698de7b9e78e0898d19fbef` contains the product branding
    corrections and 1.8.0-167 version advance; release-governance baseline
    `121f174a1a745036f2df1b9facc5bfd33bafac61` completes the branded installer
    path used by release verification.

41. Release packaging is simplified for 1.8.0-168 by removing the duplicate
    project-built source ZIP. The deleted `packaging/build-source-zip.sh` path is
    now guarded by architecture and release-contract regressions so it cannot be
    silently reintroduced. GitHub's standard immutable tag archives remain the
    source-code download links, while Defragmenter publishes only the generic
    Debian package, hardware-native local installer and their SHA-256 manifest.
    No filesystem parser, analyser, planner, writer, journal, recovery path,
    target-safety control or installed compatibility identifier changed. Exact
    source and release-governance baseline
    `0c72737a9a16f1fc46ed66f1ccb466ea3a45e5f2` passed the complete 33-test
    native/filesystem/GUI/release suite before this audit baseline was advanced.

42. Defragmenter 1.8.0-169 replaces the legacy multicolour grid emblem with
    the new black/electric-blue Defragmenter product icon. Packaging now installs
    the raster icon under the established `io.github.linuxdefragger` icon name;
    the desktop launcher, main GTK window and LINK-standard About dialog all
    resolve that same icon identity. The superseded SVG is removed and
    regressions enforce both the installed PNG and explicit main-window icon
    binding. Project CI no longer imposes per-file SPDX/licence metadata or a
    binary sidecar requirement. This is presentation/packaging-only: no filesystem analyser,
    planner, writer, transaction, recovery, target-safety or compatibility
    identifier semantics changed. Exact source baseline
    `5ae632a7c4d8489167a399da48249355cf6b0014` contains the icon integration and
    removal of project licence-enforcement checks.

43. Defragmenter 1.8.0-170 corrects the Linux icon integration using the exact
    user-supplied black/electric-blue artwork rather than a substituted render.
    Packaging now installs that PNG in the 256x256 hicolor application slot,
    the desktop launcher and main GTK window retain the established icon name,
    and the About dialog loads the packaged PNG directly at 96 px before using
    the icon-theme fallback. The application also publishes the same icon name
    as GTK's default window icon. Regression tests enforce the packaged path,
    direct About-dialog load and application/window bindings. No filesystem
    analyser, planner, writer, transaction, recovery or target-safety semantics
    changed. Exact source baseline
    `7b041922a1d0097e1c29f67dc1554ff209f19bf0` contains the correction.

## Shared Common dependency

The original 1.8.0-140 audit consumed Infiltratr Common 1.15.0 at exact commit
`d623410f55a071020539fae3f47682896473bd6f`.

The current 1.8.0-170 audit extension is bound to Defragmenter source baseline
`7b041922a1d0097e1c29f67dc1554ff209f19bf0`. Release qualification rejects any later change beneath the
runtime, native build, Common or packaging trees until the source audit baseline
is explicitly advanced. Release-governance workflows are independently bound to
`0c72737a9a16f1fc46ed66f1ccb466ea3a45e5f2`; changes beneath `.github/workflows`
likewise require the governance audit baseline to be advanced. The source baseline
validates Infiltratr Common 1.19.2 at exact commit
`44409af17c89b6ece6b4bcb2c0c133213c695c23`. CMake, the gitlink and the local
compiler/installer all verify that same version and commit rather than accepting
an unconstrained checkout.

The consolidation moves generic exact numeric and binary-quantity parsing,
production endian decoding, checked allocation arithmetic and geometric growth,
percentage calculation, POSIX lexical basename/realpath handling, JSON escaping,
exact sequential/positioned I/O, atomic recovery-state publication and durable
recovery-state removal into Common. Test Media consumes the same primitives
where their contracts match. Filesystem record formats, validation, transaction
stages, geometry, relocation policy, recovery binding, accepted operation
semantics and user-facing failure policy remain Defragmenter-owned. Recovery paths
retain byte-exact persisted path values; only generic mechanics are shared.

## Release controls and decision

The active protected-main ruleset enforces deletion protection,
non-fast-forward protection and linear history. It intentionally does not require
a branch status check or bypass actor because this repository uses a direct-main
workflow. Publication safety is enforced separately: the release workflow only
runs after a successful push-triggered Project quality gate for the exact current
`main` commit, rechecks `origin/main`, verifies the permanent history rules,
verifies both audit baselines, rejects mismatched tags or an existing release,
and publishes versioned assets only from that exact commit.

APT publication is deliberately separate from GitHub release creation. A
successful Build and publish release workflow automatically triggers the APT
refresh workflow for that exact release SHA, and the same exact version/SHA can
be supplied to its manual dispatch path if central publication needs to be
retried.

Version 1.8.0-170 is explicitly authorized for release on 2026-09-18. Any later
version requires a new explicit release decision and a separate `Release <version>`
commit whose exact head passes the Project quality gate.

Defragmenter Test Media is outside the production-operation audit. It is a
deliberately destructive filesystem-manufacturing utility with independent
system/boot-disk refusal, canonical-device matching and typed confirmation. Its
OFS/FFS/SFS fixtures are regression evidence for the corresponding native
parsers and writers, but Test Media must only be pointed at media whose complete
erasure is acceptable.
