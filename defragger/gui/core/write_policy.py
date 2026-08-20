#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary fail-closed policy for unaudited raw filesystem mutation."""

from __future__ import annotations

import os
from collections.abc import Mapping


WRITE_AUDIT_OVERRIDE_ENV = "LINUX_DEFRAGGER_ENABLE_UNAUDITED_WRITES"
WRITE_AUDIT_OVERRIDE_TOKEN = "I_ACCEPT_UNAUDITED_RAW_WRITES"
WRITE_QUARANTINE_MESSAGE = (
    "Write-capable operations are quarantined pending an independent "
    "filesystem-safety audit. Analyse/Map remains available. See "
    "docs/AUDIT_STATUS.md before using disposable test media."
)


def write_audit_override_enabled(
    environment: Mapping[str, str] | None = None,
) -> bool:
    """Return whether the deliberate disposable-media override is exact."""

    source = os.environ if environment is None else environment
    return source.get(WRITE_AUDIT_OVERRIDE_ENV) == WRITE_AUDIT_OVERRIDE_TOKEN


def require_write_audit_override() -> None:
    """Reject mutation while the audit quarantine is active."""

    if not write_audit_override_enabled():
        raise RuntimeError(WRITE_QUARANTINE_MESSAGE)
