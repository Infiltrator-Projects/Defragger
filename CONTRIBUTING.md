# Contributing

## Engineering standard

Changes to Defragmenter should preserve its first-principles ownership model. Start by identifying which layer owns the behaviour and what evidence will demonstrate the change.

## Before coding

1. Read README.md, docs/ARCHITECTURE.md and docs/DESIGN.md.
2. Search for an existing implementation before creating a parallel path.
3. Keep generic shared behaviour in the appropriate first-party shared project rather than copying it.
4. Add or update regression coverage for the changed contract.
5. Update roadmap, validation or specialist documentation when support boundaries move.

## Language and dependency policy

Prefer C/C++ for first-party native code where suitable. Use platform-native language only at a platform boundary that genuinely requires it. External dependencies must have a clear contract and must not replace project-owned semantics merely for convenience.

## Verification

Run the repository's normal build and test path before publishing a change and ensure the relevant CI workflows remain green. Warnings, sanitizer failures, packaging failures and deliberately skipped mandatory evidence are not successful validation.

## Repository policy

main is the working branch. Published tags and releases are immutable source identities. Changes should be small enough that their ownership, tests and documentation can be reviewed together.
