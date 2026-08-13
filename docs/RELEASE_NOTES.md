<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Linux Defragger 1.8.0-97

- Integrated Infiltratr Common 1.4.0 at exact commit `e4547c49400875da3e1a5638366903a01374b350` as the canonical provider for generic C parsing, bounded string handling, line-end trimming, bounded `realpath`, small sysfs scalar reads, prefix matching and saturating unit conversion.
- Removed duplicated unsigned integer parsers from the EXT, NTFS, exFAT and XFS native journal workers and adopted Common range-checked parsing in FAT, exFAT, AFFS and HFS+ where explicit bounds are required.
- Preserved Defragger-specific checked-overflow arithmetic and interruption-safe raw positional I/O because their contracts differ from Common's saturating arithmetic and text-file readers.
- The native `.run` payload carries the exact pinned Common 1.4.0 source; release builds verify the pin rather than silently using an arbitrary library revision.
- Reduced repository-root clutter by moving release/test history into `docs/` and removing SPDX `.license` sidecars for known non-commentable project metadata. Those exact metadata files are covered by the project licence policy and licensing regression without separate root files.

# Linux Defragger 1.8.0-96

- Replaced the remaining bundled classic-HFS `hfsutils` dependency with a first-party native C read-only analyser. The new analyser directly parses the HFS MDB, Extents Overflow B-tree, Catalog B-tree, data/resource-fork extent chains and fragmentation state.
- Removed the complete `vendor/` tree and all installed hfsutils notices/credits. The repository now contains no vendored third-party source.
- Added a permanent classic-HFS regression that constructs a fragmented raw HFS image directly in first-party test code. Before the dependency was removed, the new analyser was also cross-checked against the previous hfsutils-backed analyser on an independently generated fragmented HFS volume and produced the same result.
- Updated the About dialog to the current `The-First-Infiltrator/Defragger` repository and removed obsolete third-party credits.
- Rewrote the README as a current-state project overview, split every read-only filesystem into an explicit support row, and moved revision-history material to this release-notes document.
- Standardised the source archive as `Defragger-${VERSION}.zip` and added a packaging regression that verifies both the filename and `Defragger-${VERSION}/` top-level directory.
- Added an architecture guard that rejects a reintroduced top-level `vendor/` source tree.
- Revision 96 is the first tag after the post-95 documentation/naming corrections, so the GitHub-generated source ZIP, tagged README and release metadata are intended to describe one exact commit.

# Linux Defragger 1.8.0-95

- Relicensed all first-party Linux Defragger source code, headers, Python modules, build scripts, packaging, tests, documentation and project metadata under `GPL-3.0-or-later`.
- Added SPDX licence identifiers to every commentable first-party text file using the native comment syntax of that format. C and C headers use exactly `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Added `.license` sidecars for first-party artefacts that cannot safely carry inline comments, including `VERSION`, JSON configuration and compressed test fixtures.
- Added the canonical GPLv3 licence text under `LICENSES/GPL-3.0-or-later.txt` and install it with packaged documentation.
- Added an automated SPDX licensing gate so future first-party files cannot be shipped without the project licence identifier.
- The temporarily vendored classic-HFS `hfsutils` source is not relicensed; it retains its upstream GPL-2.0-or-later terms until that remaining bundled-code dependency is replaced.
- No filesystem algorithms or on-disk mutation behaviour changed in this licensing-only release.

# Linux Defragger 1.8.0-94

- HFS+ and HFSX are now first-party native C filesystem engines under `gui/filesystems/hfsplus/native/`; the previous Python format implementation has been removed from the production path.
- Analyse/Map, Defrag, Growth Defrag and Recover are provided by the native worker. Growth Defrag leaves an exact 10% post-fork allocation-block reserve.
- The native parser owns the volume header, allocation bitmap, Catalog B-tree and Extents Overflow B-tree. Fragmented user forks using overflow records are relocated without B-tree rebalancing by preserving the existing overflow keys/counts and rewriting physical extent starts.
- HFS+/HFSX mutations use a sparse verified stage, payload SHA-256 verification, allocated-range source commit and incremental live-map events.
- Clean internal HFS+ journals are supported when the journal header proves there are no pending transactions (`start == end`) or the journal is explicitly marked as needing initialisation. Journal info/data blocks are fixed metadata. Dirty journals fail closed; revision 94 does not replay HFS+ transactions.
- Format identification is independent of the consistency scan, so a dirty journaled HFS+ volume remains recognisable even though Analyse/Defrag correctly refuse to trust pending on-disk transactions.
- Added native HFS+, HFSX, extents-overflow, clean/dirty journal and Recover regressions. The recovery test also fixed a journal-header parsing bug that previously made the worker unable to read its own recovery record.
- HFS+ SHA-256 verification now uses the OpenSSL EVP API and builds under the normal project `-Werror` policy without suppressing deprecation warnings.

# Linux Defragger 1.8.0-93

- Amiga OFS/FFS (DOS\0 through DOS\7) is now a first-party native C filesystem engine under `gui/filesystems/affs/native/`.
- The bundled Python `amitools` filesystem runtime has been removed from source, package installation and runtime dependencies.
- Amiga Analyse/Map now comes from the native C catalogue. Defrag, Growth Defrag, Recover and incremental live-map events are implemented.
- File headers, extension/list blocks, directories, root/bitmap structures and other Amiga metadata remain fixed while regular-file data blocks are packed around them. OFS moved data blocks have their sequence/next pointers and checksums rebuilt.
- Growth Defrag preserves an exact 10% post-file free-block reserve.
- Amiga mutations use a verified sparse working image and commit only final allocated blocks rather than rewriting free space.
- Independent OFS and FFS regression images exercise real fragmented layouts and verify that both native Defrag and Growth Defrag reach zero fragmented regular files.

# Linux Defragger 1.8.0-92

## 1.8.0-92 — NTFS exact Growth Defrag reserve contract

- Replaced NTFS mutation verification's coarse catalogue-wide `growth_10_satisfied` gate with exact plan verification.
- Growth Defrag now records every preserved non-resident primary user stream in the durable plan database with its physical mapping, payload SHA-256, and required 10% post-file reserve.
- Stage/source verification proves movable streams and preserved streams against the same contract that the planner created; every non-resident primary user stream must be accounted for by one of those sets.
- Preserved unsupported-but-safe streams remain byte-for-byte fixed; the change does not weaken the 10% reserve rule or permit fragmented unsupported primary streams.
- Added a regression guard so NTFS mutation verification cannot silently fall back to the analyser's coarse growth summary again.


## 1.8.0-91 — NTFS safe fixed-stream preservation and failure-result protocol

- Native NTFS no longer aborts the entire canonical rewrite merely because an otherwise safe user record contains a layout the current writer does not rewrite, such as an `$ATTRIBUTE_LIST` on a record whose primary stream is already physically contiguous.
- Unsupported but safe non-primary user streams (including named data streams) are preserved byte-for-byte as fixed allocation obstacles instead of being gratuitously moved.
- An unsupported primary file/directory stream is preservable only when it is a single logical segment and is already physically contiguous. Split or fragmented unsupported primary streams still fail closed before source commit.
- Growth Defrag reserves the exact 10% post-file gap around any preserved primary stream after movable streams are released; if a fixed allocation prevents that reserve, the operation still refuses to proceed.
- Canonical NTFS packing now uses a best-fit subset before fixed allocation obstacles and preserves only the mathematically unavoidable low-address suffix that cannot hold any remaining complete stream span; files are never split merely to fill such a boundary.
- Added a native regression fixture matching the real MFT-record-29 failure: a user record with `$ATTRIBUTE_LIST` plus one contiguous unnamed `$DATA` segment is preserved unchanged while other streams are compacted.
- The typed GUI worker-result protocol now accepts the explicit `failed` result status emitted by native workers, eliminating the secondary `Engine event could not be decoded` error after a legitimate worker failure.
- The C-first architecture and the production ban on external filesystem mutation/repair commands remain unchanged.

## 1.8.0-90 — EXT allocated-range persistent commit

- EXT persistent commit now derives contiguous write ranges from the final verified staged EXT block bitmap and writes only blocks allocated in the final filesystem, instead of sequentially rewriting every byte of the logical filesystem.
- Free EXT blocks are deliberately left physically untouched; the final verified block bitmap remains authoritative, so stale bytes in free space do not require erasure.
- The durable recovery cursor remains an absolute physical byte offset. Recovery reconstructs the same final allocated ranges from the persistent staged image and safely resumes at or after the last synced cursor.
- Commit progress is calculated from allocated bytes actually being persisted rather than total filesystem capacity.
- Native EXT Defrag and Growth Defrag regression tests now require allocated-range commit reporting and prove that a mostly-free 512 MiB fixture commits less than half of full capacity.
- The existing C-first architecture and production ban on external filesystem mutation/repair utilities remain unchanged.

# Linux Defragger 1.8.0-89

## 1.8.0-89 — exFAT allocated-range commit and genuine live relocation map

- exFAT persistent commit now writes only the verified FAT plus clusters allocated in the final canonical filesystem, instead of sequentially rewriting every byte of a mostly-empty partition. Blocks that become free are left physically untouched and are made free by the verified allocation bitmap/FAT metadata.
- exFAT recovery derives the same final allocated ranges from the verified persistent working image and resumes with the durable absolute commit cursor.
- Native exFAT stage construction now emits batched `@@LIVE_RANGES` events for actual old-to-new cluster placement, so the GUI allocation map updates while Defrag/Growth Defrag is being built instead of remaining static until completion.
- exFAT `@@LIVE_RESET` now uses the common filesystem-neutral live-map schema (`unit_size`, `filesystem_units`, `used_ranges`) rather than its obsolete exFAT-specific reset payload.
- Native end-to-end exFAT Defrag and Growth Defrag tests assert both incremental live events and allocated-range source-commit reporting.

# Linux Defragger 1.8.0-88

## 1.8.0-88 — XFS allocated-range commit and live relocation map

- XFS persistent commit now writes only the verified final allocated ranges instead of rewriting every byte of the filesystem. Free blocks that remain free or become free are not needlessly rewritten; XFS allocation metadata remains authoritative.
- Recovery uses the same absolute cursor over verified allocated ranges, retaining the durable staged-commit model without the full-device write amplification.
- Native XFS relocation emits batched `@@LIVE_RANGES` events while payload blocks are rearranged, so the allocation map visibly updates during Defrag and Growth Defrag.
- The final `@@LIVE_RESET` remains authoritative after metadata rebuild and source verification.

## 1.8.0-87 — XFS fixed-metadata boundary slack

- Corrects the real-device Growth Defrag failure where a one-block free suffix immediately before immovable XFS metadata was treated as a fatal canonical-layout violation.
- Keeps exact 10% Growth Defrag reserves and contiguous file objects unchanged. A fixed-metadata boundary suffix is accepted only when each such gap is smaller than the smallest complete movable span, proving that no whole file+reserve object can occupy it.
- Potentially avoidable gaps remain a hard planning failure; the writer does not fragment an object, reduce a reserve, or move fixed XFS metadata to hide the condition.
- Reports preserved boundary slack explicitly in the operation log so tiny unavoidable free regions are not mistaken for failed compaction.
- Adds native C regression coverage for the one-block Growth Defrag case exposed by the 16 GB XFS test volume.


## 1.8.0-86 — kernel-equivalent XFS clean-log selection

- Corrects the remaining XFS clean-log false negative exposed by a real unmounted XFS test volume.
- Replaces the revision-85 "record ending at the estimated head / largest LSN" selection with a physical reverse search matching `xlog_rseek_logrec_hdr`: the first record header encountered while walking backwards from the head is the candidate preceding record.
- Adds kernel-style head refinement for same-cycle and partial-record layouts. A cycle-derived head that lands in stale or partially written log space is backed to the nearest record header before cleanliness is evaluated.
- Matches `xlog_check_unmount_rec` for the final cleanliness decision: once the preceding record is proven complete, ends exactly at the physical head and contains one operation, `XLOG_UNMOUNT_TRANS` is the authoritative clean-unmount marker. Client ID and operation length remain diagnostic fields rather than invented extra gates.
- Dirty or structurally ambiguous logs still fail closed; no XFS kernel driver, mount, loop device, XFS ioctl or external XFS utility is used.
- Failure diagnostics now report the calculated head, preceding header/data block, cycle, record length, header size, operation count, client, flags and operation length so real-device format cases can be identified without guessing.
- Adds native regression coverage for same-cycle head refinement and the kernel-equivalent unmount-flag criterion in addition to pre-wrap and wrapped logs.

## 1.8.0-85 — XFS physical-log head correctness

- Replaced the raw XFS writer's "largest LSN wins" clean-log test with a physical circular-log head proof based on XFS cycle stamps.
- The clean-unmount marker is accepted only when the complete record preceding the calculated physical log head is a single unmount operation.
- Added native white-box regression coverage for clean pre-wrap and wrapped logs.
- This revision was an intermediate correction: real-device testing showed that its head-record selection and clean-marker test were still stricter/different from XFS recovery semantics; revision 86 supersedes those parts.

## 1.8.0-84 — overlap-aware partition mount safety

- Corrects the mount-safety graph so an unmounted target partition is no longer rejected merely because a disjoint sibling partition on the same disk is mounted.
- Whole-device mutation still rejects mounted child partitions, and partition mutation still rejects the target itself, containing whole-device mounts, holders, slaves, loop mappings and other genuinely overlapping mappings.
- Applies the same overlap semantics in both the Python operation preflight and the native C raw-device safety layer.
- Adds a regression test that explicitly distinguishes a mounted sibling partition from an overlapping target/mapping.


Revision 83 converts the remaining three active Python mutation engines at the top of the C-first migration: EXT2/3/4, NTFS and exFAT. It also makes the no-external-filesystem-tool rule a build-enforced production invariant rather than a convention.

## EXT2/3/4 native C engine

- Replaced the production Python EXT parser/planner/writer transaction with native C under `gui/filesystems/ext4/native/`.
- The worker links `libext2fs` directly in-process; production no longer launches filesystem command-line utilities for imaging, resizing, verification or repair.
- Native C owns catalogue construction, deterministic placement, cycle-safe raw relocation, inode/block-map updates, exact 10% Growth Defrag reservations, staged verification, durable commit and recovery.
- A generated modern EXT4 fixture completed Defrag and Growth Defrag through the native worker and rescanned at zero fragmented files/directories.

## NTFS native C engine

- Removed the production Python NTFS bitmap/catalogue/codec/placement/record/transaction/volume/writer implementation and compatibility engine wrapper.
- Native C now owns boot/MFT parsing, update-sequence fixups, runlists/mapping pairs, `$Bitmap`, deterministic placement, payload hashing, MFT mapping rewrites, staged verification, commit and recovery.
- Native Defrag and Growth Defrag fixtures complete with zero remaining fragmentation and exact 10% reserve verification.

## exFAT native C engine

- Removed the production Python exFAT format/model/planner/transaction/volume/writer implementation and compatibility engine wrapper.
- Native C now validates the boot regions, FAT, allocation bitmap, upcase table and directory entry sets, then builds a verified sparse canonical working image without requiring free in-volume staging clusters.
- Native Defrag and Growth Defrag fixtures reduce fragmented files/directories to zero while preserving filesystem identity and verifying the exact 10% reserve.

## Regression prevention

- Added a production architecture test that rejects known external filesystem mutation/repair tools, loop-device orchestration and equivalent command delegation in production source or package dependencies.
- Removed the revision-82 architecture assertion that incorrectly required the old EXT command-line-tool workflow.
- The old EXT, NTFS and exFAT Python mutation implementations are physically absent from production; the native workers are replacements, not wrappers or fallbacks.
- The native workers accept the same standard operation arguments emitted by the GUI, including live-map options, and pass Defrag/Growth Defrag through that actual command contract.
- The complete first-party C build remains subject to warnings-as-errors.

---

# Linux Defragger 1.8.0-82

Revision 82 completes the single-plugin consolidation and makes XFS the first complex raw filesystem engine to move completely to the C-first model.

## XFS is now authoritative native C

- The production XFS package now contains only a thin Python `plugin.py` adapter plus the native C implementation under `gui/filesystems/xfs/native/`.
- Removed the Python XFS geometry parser, B+tree walkers, extent codec, planner, relocator, metadata rebuilder, staging layer, transaction engine and writer entry point.
- Removed the compatibility `gui/xfs_engine.py` launcher.
- Native C now owns XFS identification, allocation-group scanning, inode/BMBT decoding, fragmentation analysis, deterministic Defrag/Growth Defrag planning, SHA-256 payload verification, cycle-safe block permutation, direct inode extent rewriting, `bnobt`/`cntbt`/`rmapbt` rebuild, AGFL management, CRC verification, clean-log validation, durable staging/commit journal and recovery.
- `gui/filesystems/xfs/plugin.py` now only launches the C worker and translates its JSON analysis into the common GUI map schema.
- The XFS C targets compile with the same warnings-as-errors policy as the shared core and FAT worker.
- Native tests cover CRC32c, placement, actual disposable-image block relocation, unsupported-feature gates and multi-level rmap separator/CRC/sibling validation. A synthetic minimal-XFS image also exercises the native analyser through the GUI adapter.

## Architecture inherited from revision 81

Revision 82 keeps revision 81's single authoritative `gui/filesystems/<format>/` hierarchy and one GUI filesystem registry. Native C remains private implementation code owned by its filesystem package rather than a parallel plugin system.

---

# Linux Defragger 1.8.0-81

Revision 81 removed the parallel native filesystem-plugin hierarchy and made `gui/filesystems/<format>/` the sole filesystem ownership and dispatch model.

## Single authoritative filesystem hierarchy

- `gui/filesystems/<format>/` is now the only filesystem plugin hierarchy.
- `gui/backends/registry.py` is the only filesystem registry and dispatch authority.
- Removed the parallel `src/filesystems/` hierarchy.
- Removed the native `src/engine/` filesystem registry and `LdFilesystemPlugin` ABI.
- FAT remains native C, but its implementation now lives under `gui/filesystems/fat/native/` and is invoked only as a private worker declared by the GUI FAT plugin.
- Classic HFS owns its native read-only analyser under `gui/filesystems/hfs/native/`.
- `src/core/` remains as filesystem-neutral C infrastructure for raw I/O, device safety, staging, Stop handling and runtime helpers.
- Architecture tests now fail if a parallel native filesystem registry or `src/filesystems/` hierarchy is reintroduced.

This is an ownership and dispatch cleanup, not a rewrite of the FAT algorithms. The existing FAT parser, planner, journal, relocation and analysis modules remain native C and retain their tests.

---

# Linux Defragger 1.8.0-80

Revision 80 replaces the XFS kernel-assisted mutation design with an offline raw userspace XFS v5 writer.

## XFS filesystem-driver independence

- Removed the production XFS loop-mount, mounted-image, FIEMAP and XFS filesystem-ioctl relocation path.
- Removed the production `xfs_repair`/xfsprogs mutation dependency.
- XFS analysis, Defragment, Growth Defrag, verification and recovery now operate through raw unmounted-device/image I/O and project-owned XFS format code.
- Regular-file data extents are the only movable XFS payload. Directories, inode allocation trees, attribute forks, journal blocks, refcount metadata and unrelated metadata are preserved.

## Raw XFS metadata transaction

- Rewrites supported direct inode extent forks and recalculates v3 inode CRC32c.
- Rebuilds affected allocation-group `bnobt`, `cntbt` and `rmapbt` structures internally.
- Uses existing allocation-tree blocks plus AGFL reserve blocks when B-tree topology must grow.
- Updates AGF roots, levels, free-space counters, B-tree block counters and reverse-map block counts.
- Recalculates touched v5 metadata CRCs and verifies disk address, owner, level, separator and sibling invariants.
- Corrects modern AGF field offsets and the v5 AGFL `XAFL` header.
- Corrects multi-level rmap internal keys to store an interleaved low/high key pair for each child.
- Handles rmap high keys correctly for special metadata owners and BMBT ownership records.

## Fail-closed preflight

Revision 80 refuses raw XFS mutation when the filesystem has an external log, realtime data device, dirty/unprovable internal log, needs-repair state, unsupported regular-file bmap tree, movable reflink/nodefrag state, or newer incompatible feature semantics the writer does not implement. Read-only analysis may remain broader than write support.

## Testing

The reconstructed focused suite passes synthetic raw XFS v5 allocation metadata tests including 400 moved regular files across two allocation groups, multi-level rmap growth, AGFL reserve consumption and complete post-rebuild verification. The architecture test rejects production references to xfsprogs, XFS filesystem ioctls, FIEMAP, loop setup and XFS mounting.

The original lost revision-80 session also recorded a separate 512 MiB/four-allocation-group modern XFS integration run with 400 files and two successful independent `xfs_repair -n` validations. That external test cannot be rerun in the current packaging environment because the original working tree and its local xfsprogs test binaries are no longer present; it is historical evidence, not a new validation claim for this reconstructed byte set.

## Provenance

The original revision-80 working tree was lost before its three release artifacts were built. This reconstruction starts from the last source archive available here, revision 62, and reapplies the surviving revision-80 XFS design and safety findings. It therefore cannot assert inclusion of unrelated source changes from missing revisions 63 through 79.
