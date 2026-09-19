# Design

## First-principles position

Defragmenter starts from the behaviour the product must own, studies standards and mature implementations as evidence, and then chooses the strongest justified design rather than copying an existing product or preferring novelty for its own sake.

## Goals

- make physical placement deterministic and inspectable
- never report unsupported or uncertain mutation as success
- persist enough recovery state before authoritative writes
- separate generic device safety from filesystem-specific rules

## Non-goals and limits

Supporting read-only analysis of a format does not imply write support. The project does not use a mounted kernel driver or external defragmenter as a hidden mutation backend.

## Dependency policy

C and C++ are preferred for first-party native implementation where they fit the problem. Platform frameworks and external libraries are used when their documented contract is the stronger engineering choice. A dependency must not silently become the source of product policy, and exact first-party dependencies are pinned where reproducibility requires it.

## Failure philosophy

Unsupported, unavailable or unverified states are represented explicitly. The project prefers a visible refusal or unavailable state to guessed success. Destructive or irreversible behaviour requires a stronger evidence bar than read-only behaviour.

## Decision quality

Design changes should identify the problem, alternatives, evidence, trade-offs and validation method. "Newer" is not a sufficient reason to replace a proven approach. A replacement should improve correctness, safety, performance, resilience, usability or maintainability without weakening an established contract.
