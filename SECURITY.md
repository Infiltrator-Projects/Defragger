# Security

## Supported source

Security fixes target current `main` and, where appropriate, the latest published release. Older releases should not be assumed to receive backports.

## Reporting

Do not open a public issue for a vulnerability that could enable unintended raw-device access, destructive writes, privilege-boundary bypass, sensitive local-data exposure, package/release compromise or reliable memory corruption.

Use GitHub private vulnerability reporting when available. Otherwise contact `infiltratr@yandex.com` with the subject `Defragmenter security report`.

Include the affected version/commit, Linux distribution, target type, privilege context, impact, reproduction steps and relevant sanitised logs. Remove unrelated credentials or private data.

## Security-sensitive boundaries

Reports are especially relevant for:

- raw-device target identity, mounted-overlap checks and exact write confirmation;
- privileged-helper command/path validation and recovery-state ownership;
- filesystem metadata parsing, bounds/integer handling and malformed-input behaviour;
- transaction durability, Recover and Stop boundaries;
- temporary/staging files and path/symlink handling;
- native memory-safety faults reachable from filesystem input;
- dependency pinning, package integrity and release automation.

The separate Test Media utility is intentionally destructive when pointed at explicitly selected sacrificial media; bypass of its system-disk protections is security-relevant.

## Response and validation

Security defects are correctness defects. Reproduce the issue, add the narrowest useful regression where practical, fix the owning boundary and validate at the level actually affected. Parser faults need malformed-input evidence; destructive-operation faults need target/transaction evidence; packaging faults need package/release evidence.

## Disclosure

Public details should follow a fix or clear mitigation so affected and corrected source identities are known. Testing must be limited to systems and data the reporter is authorised to use.
