# Documentation

This directory is the canonical documentation entry point for Defragmenter.

## Canonical baseline

- [Architecture](ARCHITECTURE.md) — ownership, layers, dependencies and system boundaries.
- [Design](DESIGN.md) — safety model, trust boundaries, invariants, trade-offs and implementation architecture.
- [Decisions](DECISIONS.md) — durable architectural decisions, alternatives and consequences.
- [Roadmap](ROADMAP.md) — current foundation, near-term priorities and longer-term direction.
- [Validation](VALIDATION.md) — qualification methodology, traceability and evidence boundaries.
- [Project README](../README.md) — product overview, capabilities, build/use entry point and engineering ethos.
- [Changelog](../CHANGELOG.md) — user-visible and contract-relevant change history.
- [Contributing](../CONTRIBUTING.md) — development, ownership and verification rules.
- [Security](../SECURITY.md) — vulnerability reporting and supported-source policy.

## Specialist documentation

- [Audit status](AUDIT_STATUS.md) — release-specific safety audit evidence and exact audited source.
- [References](REFERENCES.md) — external specifications, format references and engineering sources.
- [Implementation README](../defragger/README.md) — canonical source-tree/build entry point for the application directory.

## Authority

Architecture defines ownership. Design defines safety and present rationale. Decisions preserve long-lived choices and rejected alternatives. Roadmap states direction. Validation defines the evidence methodology. Audit Status records release-specific audit evidence.

Do not duplicate those roles in new Markdown files. Filesystem-specific implementation detail belongs with the owning source/package unless it requires a maintained cross-format contract.
