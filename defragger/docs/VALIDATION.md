<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Defragmenter validation methodology

## Purpose

This document explains how the project obtains evidence for its correctness and
safety claims. Verification asks whether the implementation conforms to the
stated design invariants; validation asks whether those invariants and tests are
credible for the intended offline-defragmentation use case. Neither testing nor
sanitizers constitute a formal proof, so this document also records the
remaining limits of the evidence.

The current release-specific decision and exact audited commits are recorded in
[AUDIT_STATUS.md](AUDIT_STATUS.md). Architectural requirements are defined in
[DESIGN.md](DESIGN.md).

## Evidence strategy

The project uses layered evidence because no single test technique is adequate
for raw filesystem mutation:

1. **Static/build enforcement** catches compiler diagnostics, architecture drift,
   licensing drift and prohibited external mutation dependencies.
2. **Native unit tests** exercise parsers, checksums, geometry, vector/range
   helpers and filesystem-specific metadata transformations.
3. **Disposable-image integration tests** create known fragmented filesystems,
   run production workers and reopen the result.
4. **Independent post-operation checks** verify payload identity, contiguity,
   allocation gaps and Growth Defrag reserve rules using code paths distinct
   from the mutation decision where practical.
5. **Recovery/fault-injection tests** interrupt transactions at durable
   checkpoints and verify that source state is unchanged, valid or recoverable.
6. **GUI/service tests** verify command construction, privilege boundaries,
   protocol parsing, Stop behaviour and presentation-state transitions.
7. **Dynamic instrumentation** runs the native suite with AddressSanitizer and
   UndefinedBehaviorSanitizer.
8. **Packaging/release tests** rebuild the distributable forms and bind
   publication to the exact quality-gated commit.

The acceptance rule is intentionally asymmetric: evidence may enable a
well-specified format/feature combination, but uncertainty disables mutation.

## Traceability from claims to evidence

| Design claim | Principal enforcement/evidence |
| --- | --- |
| Mounted/overlapping targets are not mutated | `tests/test_safety.py`, native architecture checks, descriptor-level mounted-state recheck |
| Target replacement is detected across open/commit boundaries | `src/core/ld_device.*`, filesystem target-identity checks, safety/native tests |
| Unsupported/malformed metadata fails closed | filesystem-native tests, negative fixtures, architecture tests |
| Growth Defrag leaves the exact required reserve | FAT/exFAT/NTFS/EXT/XFS/AFFS/SFS/HFS+ integration verification and `verify_growth_*` scripts |
| Stop does not abandon an unsafe intermediate state | transaction/native recovery tests and worker Stop-path regressions |
| Interrupted authoritative writes retain recovery state | `tests/test_transactions.py` plus filesystem recovery suites |
| Writer success requires reopened verification | disposable-image tests and filesystem-specific verification routines |
| Generic infrastructure does not silently fork into duplicate implementations | `tests/test_architecture.py` and Common pin checks |
| Published artifacts correspond to the tested source | `tests/test_release_gate.py`, release-artifact tests and exact-head release workflow |

This table identifies the primary evidence, not every regression covering a
claim.

## Build and static qualification

The hosted quality gate configures the project with
`LD_ENABLE_WERROR=ON`, builds first-party C with warnings treated as errors,
then runs the aggregate CTest suite. Python type/architecture tests verify
module boundaries and worker contracts. SPDX and no-external-filesystem-tool
tests enforce licensing and the raw-userspace design boundary.

The Common dependency is pinned by version and exact commit in CMake and local
packaging. This prevents a successful build from silently changing the generic
parsing/I/O/arithmetic semantics beneath an audited Defragmenter source tree.

## Disposable filesystem images

Write-capable engines are exercised on disposable images or sacrificial media
whose expected contents are known. Tests deliberately manufacture
fragmentation, perform Defragment or Growth Defrag through the production worker,
then reopen the resulting filesystem.

Useful test-oracle separation is maintained where practical:

- the writer's placement decision is not accepted as proof that its own output
  is correct;
- verification scripts reconstruct allocation/layout properties from the result;
- deterministic payload hashes or byte patterns detect silent data movement
  errors;
- final filesystem-specific scans validate metadata and allocation consistency.

When the same parser must necessarily participate in both production and test
setup, additional structural checks and independently generated fixtures are
used to reduce common-mode error. This is weaker than a wholly independent
implementation and is treated as such.

## Failure and recovery validation

Transaction tests inject deterministic failures immediately before and after
durable journal publications. Filesystem-specific recovery suites additionally
exercise corrupt/truncated stages, target mismatch, unsupported or legacy
journals, resume after partial progress and cleanup only after successful final
verification.

The expected post-interruption states are deliberately limited to:

- no source write occurred and temporary state may be discarded;
- the filesystem is already valid and the transaction can be verified/cleaned;
- durable transaction material remains sufficient for Recover.

A test that merely observes a non-zero exit code is not sufficient for a
destructive path; source bytes or transaction artefacts must also satisfy the
relevant safety invariant.

## Sanitizer qualification

The hosted native lane uses ASan and UBSan to detect memory-safety and undefined
behaviour defects exercised by the suite. Sanitizers improve confidence in
executed paths but do not establish absence of defects in unexecuted paths,
prove crash consistency, or validate filesystem-format semantics.

## Reproducibility

From the canonical project directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The aggregate harness is also available through CTest and the GitHub-hosted
quality workflow. Release artifacts are rebuilt from the exact release commit;
the source archive, Debian package, native local installer and SHA-256 manifest
are then published together.

## Validation limits and residual risk

The current evidence does **not** claim:

- mathematical proof of every filesystem transformation;
- resilience to a malicious/root-compromised operating system;
- recovery from hardware or firmware that falsely acknowledges persistence,
  silently corrupts data, or changes media outside the software-visible model;
- support for every feature combination permitted by every filesystem
  specification;
- equivalence between a synthetic fixture and all real-world filesystem
  histories;
- that ASan/UBSan can detect defects on paths the tests do not execute.

These limitations are reasons for the project's fail-closed feature gates,
verified-backup recommendation and requirement to qualify destructive changes
on disposable media before important data is used.

## Adding or changing a writer

A write-capable change is not complete until its evidence changes with it.
At minimum, the change should identify the affected invariant, add or update a
negative/fail-closed case, exercise a successful disposable-image path, verify
the final image independently, and exercise interruption/recovery if the
transaction boundary changes. The safety audit baseline must then be advanced
to the exact qualified source commit before a release is authorized.
