# Validation

## Purpose

Validation distinguishes implemented behaviour from behaviour that has actually been demonstrated. A successful build proves compilation; it does not by itself prove filesystem correctness, crash consistency or safe recovery.

Defragmenter therefore uses stronger evidence for destructive operations than for read-only analysis. The current release-specific safety decision and exact audited baselines are recorded in [AUDIT_STATUS.md](AUDIT_STATUS.md).

## Automated evidence

The project quality gate combines:

- warnings-as-errors native builds;
- parser, geometry, checksum and allocation-model tests;
- disposable filesystem-image mutation tests;
- target-safety, privilege, Stop and transaction regressions;
- GUI/service and typed worker-protocol tests;
- architecture/Common/release-contract tests;
- AddressSanitizer and UndefinedBehaviorSanitizer qualification;
- package/native-installer construction from the exact tested source.

Automated checks cover ordinary behaviour, important boundaries, malformed/error cases and release/package contracts appropriate to the affected subsystem.

## Destructive-path evidence

A write-capable change is expected to demonstrate more than process success. Where the filesystem contract permits it, tests manufacture a known fragmented image, invoke the production worker, reopen the result and verify payload identity plus the required allocation layout.

Recovery tests inject failure around durable transaction boundaries and accept only three classes of result: no authoritative source write occurred, the filesystem is already valid, or durable state remains sufficient for Recover.

Growth Defrag tests verify the exact 10% post-file reserve rather than treating "some free space" as equivalent.

The mutation path is not accepted as its own sole oracle where a separate structural or payload check can be used.

## Manual and environment-dependent evidence

Synthetic images and hosted runners cannot prove every storage-controller, kernel, privilege-manager or real-media interaction. Live testing on sacrificial media is therefore separate evidence for environment-dependent behaviour.

Manual evidence must be described at the level actually observed. A fixture, simulator or mocked failure is not physical-media proof.

## Release criterion

The exact revision intended for release must pass the required Project quality gate. Release assets must be derived from that revision, the audit must name the current version and exact audited source/governance baselines, and documentation must not advertise known-failing or merely planned write support as complete.

A release gate also verifies the exact pinned Common dependency and rejects audited production or workflow drift beyond the recorded baselines.

## Regression rule

Every reproducible defect should gain the narrowest useful permanent regression. Changes to a writer should update the evidence for the affected safety boundary, including a fail-closed case and interruption/recovery coverage when the transaction boundary changes.

Tests are part of the product contract, not disposable scaffolding.

## Limits

The evidence is not a mathematical proof and does not establish correctness for untested feature combinations, compromised privileged environments, or hardware/firmware that falsely acknowledges persistence. Those limits are why unsupported states fail closed and destructive qualification uses verified backups or sacrificial media.
