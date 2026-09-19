# Design

## First-principles position

Defragmenter starts from on-disk filesystem structures, durability requirements and the behaviour the product must own. Specifications, mature implementations and external tools are evidence to study; they are not runtime authorities for Defragmenter's placement or recovery policy.

## Goals

- fail closed when metadata, geometry, target identity or feature support is uncertain
- keep filesystem parsing, placement and recovery with the filesystem that owns those rules
- make every authoritative write either valid, unchanged or recoverable after interruption
- verify completed mutation by reopening and rescanning the target
- keep generic mechanisms in Common without exporting Defragmenter-specific policy
- bound arithmetic, allocation growth and hostile metadata traversal

## Non-goals and limits

Defragmenter is not a general filesystem repair suite, an online defragmenter or a replacement kernel filesystem driver. It does not promise write support for every legal feature combination of every supported format.

Read-only analysis may cover formats for which mutation is intentionally unavailable. Unsupported means no write.

## Language and dependency policy

C is preferred for low-level parsing, native filesystem engines and direct storage work. Python remains appropriate for GTK/backend orchestration where it does not duplicate filesystem policy.

Platform libraries and in-process filesystem libraries are used when their documented contract is the stronger engineering choice. External command-line repair or defragmentation programs are not part of production mutation paths.

The reusable first-party dependency is pinned to Infiltratr Common 1.19.3 at exact commit `de7251ce12ed176048df1bad05ef7e4d0db7e9ec`. Generic parsing, arithmetic, byte-order, path, allocation-growth, JSON and exact-I/O mechanisms belong there when their contracts match. Filesystem geometry, placement, target safety and transaction/recovery policy remain local.

## Failure philosophy

A destructive operation has a higher evidence bar than a read-only operation. Unknown features, impossible geometry, stale recovery state, target replacement, mounted overlap, failed staging or failed verification stop mutation rather than triggering a best-effort guess.

Stop is cooperative and is honoured only where on-disk state is valid or recoverable. A writer returning zero is not enough; the final filesystem state must satisfy the independently checked postcondition.

## Decision quality

Design changes should identify the owning layer, the safety or correctness property being changed, alternatives considered, trade-offs and the evidence that will validate the change.

A replacement earns its place by improving correctness, recoverability, performance, resilience, usability or maintainability without weakening an established safety contract. "Newer", "shorter" or "more shared" is not sufficient by itself.
