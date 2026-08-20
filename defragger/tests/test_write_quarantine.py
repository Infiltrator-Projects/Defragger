#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The audit quarantine must fail closed and require an exact override."""

from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gui"))

from core.write_policy import (
    WRITE_AUDIT_OVERRIDE_ENV,
    WRITE_AUDIT_OVERRIDE_TOKEN,
    require_write_audit_override,
    write_audit_override_enabled,
)


def main() -> None:
    previous = os.environ.pop(WRITE_AUDIT_OVERRIDE_ENV, None)
    try:
        assert not write_audit_override_enabled()
        assert not write_audit_override_enabled({WRITE_AUDIT_OVERRIDE_ENV: "yes"})
        try:
            require_write_audit_override()
        except RuntimeError as exc:
            assert "quarantined" in str(exc).lower()
            assert "Analyse/Map" in str(exc)
        else:
            raise AssertionError("mutation quarantine failed open")

        os.environ[WRITE_AUDIT_OVERRIDE_ENV] = WRITE_AUDIT_OVERRIDE_TOKEN
        assert write_audit_override_enabled()
        require_write_audit_override()
    finally:
        if previous is None:
            os.environ.pop(WRITE_AUDIT_OVERRIDE_ENV, None)
        else:
            os.environ[WRITE_AUDIT_OVERRIDE_ENV] = previous
    print("write-operation audit quarantine tests passed")


if __name__ == "__main__":
    main()
