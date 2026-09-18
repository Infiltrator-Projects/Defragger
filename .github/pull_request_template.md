<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!--
This template is for external or exceptional pull requests. The maintainer
workflow is intentionally direct-to-main and is release-gated by the permanent
quality gate; a PR branch is not the release authority.
-->

## Purpose

Describe one bounded change and why it is necessary.

## Evidence

- [ ] The exact PR head completed the permanent quality gate.
- [ ] The result links below belong to the current head commit.
- [ ] No branch deletion, release publication, or unrelated repository administration is included.
- [ ] Raw-write changes include disposable-image tests and independent post-operation verification.
- [ ] A reviewer other than the author has inspected destructive paths.

Exact-head commit:

Quality-gate run:

## Filesystem safety

List affected on-disk formats, transaction stages, recovery behaviour, and fail-closed conditions. Write `read-only` when the change cannot mutate a filesystem.
