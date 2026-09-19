# Architecture

## Purpose

Defragmenter is an offline filesystem allocation analyser and defragmenter that owns parsing, placement planning, staging, recovery and verified mutation rather than delegating write behaviour to mounted filesystem drivers or external repair tools.

## System decomposition

- GTK application
- filesystem-neutral raw-I/O and safety core
- per-filesystem analysis/mutation engines
- persistent recovery/staging machinery
- test-fixture generators
- quality-gate and safety regression suite
- pinned Common primitives

## Ownership boundaries

The operating system supplies block I/O. Defragmenter owns filesystem interpretation, placement, mutation and recovery for the formats it explicitly supports. Unsupported layouts fail closed.

The architectural rule is that mechanisms may come from an operating system, toolkit, shared first-party library or documented external API, but product semantics remain with their owning repository. Dependencies are accepted because their contract is useful, not as a substitute for understanding the behaviour being exposed to users.

## Source of truth

Implementation and tests define executable behaviour. This document defines module ownership and dependency direction. Specialist documents refine particular subsystems but must not create a competing architecture.

## Change rules

Cross-layer shortcuts require a documented reason. Platform handles and toolkit objects should not leak into portable/domain contracts. Failure states must remain representable across boundaries rather than being converted into plausible-looking values.

## Specialist documents

- defragger/docs/AUDIT_STATUS.md
- defragger/docs/DECISIONS.md
- defragger/docs/DESIGN.md
- defragger/docs/REFERENCES.md
- defragger/docs/VALIDATION.md
