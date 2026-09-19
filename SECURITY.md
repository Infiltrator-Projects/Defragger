# Security

## Scope

Security-relevant defects include memory-safety errors, unsafe parsing of untrusted or device-provided data, privilege-boundary mistakes, destructive-operation guard failures, insecure file/path handling, unsafe dynamic loading, and any condition that can turn malformed input into unintended code or data access.

## Reporting

Do not publish sensitive exploit details in a public issue. Use GitHub's private vulnerability-reporting or security-advisory mechanism for this repository when available.

Include the affected revision, platform, reproduction steps, expected and observed behaviour, and the smallest known impact boundary.

## Response

Security defects are treated as correctness defects. Where practical, the fix should include a regression test that would have failed before the correction. The validation scope must match the affected boundary: parser fixes need malformed-input tests; privilege/destructive-operation fixes need policy tests; platform defects need platform evidence.

## Supported source

Current main and the current released line are the primary maintained sources unless a release explicitly states otherwise. Superseded development snapshots are not independently maintained.

## Disclosure

Public details should follow a fix or clear mitigation so users can identify the affected and corrected release identities.
