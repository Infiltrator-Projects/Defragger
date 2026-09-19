# Validation

## Purpose

Validation distinguishes implemented behaviour from behaviour that has actually been demonstrated. A build proves compilation; it does not by itself prove runtime, hardware or integration correctness.

## Automated evidence

The repository currently uses:

- .github/workflows/quality-gate.yml
- .github/workflows/local-quality.yml
- .github/workflows/release.yml
- .github/workflows/apt-refresh.yml
- .github/workflows/windows-runner-smoke.yml

defragger/tests contains native, filesystem, GUI, architecture, safety and fixture-driven regression tests across supported formats and operation modes.

Automated checks should cover ordinary behaviour, important boundaries, malformed/error cases and release/package contracts appropriate to the project.

## Manual and environment-dependent evidence

Destructive media operations and recovery from real host interruption remain hardware/manual evidence. Test Media must be exercised only on sacrificial targets.

Manual evidence supplements automation and must be described at the level actually observed. A simulator, fixture or mocked provider must not be described as physical-device proof.

## Release criterion

The exact revision intended for release must pass the required automated gates. Release assets must be derived from that revision, and documentation must not advertise known-failing or merely planned behaviour as supported.

## Regression rule

Every fixed defect should gain the narrowest useful permanent regression check when reproducible. Tests are part of the product contract rather than disposable scaffolding.
