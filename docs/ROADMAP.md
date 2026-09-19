# Roadmap

This is a direction document, not a dated promise. The released source and tests define what is actually supported.

## Current foundation

- maintain broad read-only allocation analysis and the explicitly qualified write engines
- keep exact target confirmation, mounted-target refusal and recovery invariants enforced
- run the full filesystem/GUI/safety regression gate

## Near-term priorities

- expand write support only where a complete staging/recovery contract can be proved
- continue reducing duplicated filesystem-neutral mechanics
- keep audit status and validation evidence aligned with actual implementation

## Longer-term direction

- add deeper format coverage when corruption/recovery behaviour is understood well enough to remain fail-closed
- improve performance without weakening deterministic placement or recovery semantics

## Admission rule

A proposed capability enters the roadmap only when its ownership is clear and there is a credible way to validate it. Features that require pretending uncertain behaviour is known do not qualify.

## Completion rule

An item is complete when implementation, tests, user-visible behaviour and maintained documentation agree. A checkbox or release number cannot substitute for missing evidence.
