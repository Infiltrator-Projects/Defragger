#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the shared typed GUI transaction primitive.

Filesystem mutation journals for C-first engines are deliberately implemented
inside their native workers; this test covers only the filesystem-neutral GUI
transaction helper.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
GUI = ROOT / "gui"
sys.path.insert(0, str(GUI))

from core.transaction import (  # noqa: E402
    InjectedTransactionFailure,
    TransactionFailureInjector,
    TransactionJournal,
    TransactionSchema,
)


def schema() -> TransactionSchema:
    return TransactionSchema(
        filesystem="test", marker_key="kind", marker_value="test-transaction",
        schema_versions=frozenset({1}), phase_key="phase", target_key="target",
        phases=frozenset({"new", "copied", "complete"}),
        required_keys=frozenset({"checksum"}),
        transitions={"new": frozenset({"copied"}), "copied": frozenset({"complete"}), "complete": frozenset()},
    )


def payload(phase: str) -> dict[str, object]:
    return {"schema": 1, "kind": "test-transaction", "phase": phase,
            "target": "/tmp/test.img", "checksum": "abc"}


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "journal.json"
        journal = TransactionJournal(schema())
        record = journal.record(payload("new"))
        journal.save(path, record)
        assert journal.load(path).phase == "new"
        record = journal.transition(path, record, "copied")
        assert journal.load(path).phase == "copied"
        journal.remove(path)
        assert not path.exists()

        before = TransactionFailureInjector("before-persist:new")
        try:
            TransactionJournal(schema(), checkpoint=before).save(path, TransactionJournal(schema()).record(payload("new")))
        except InjectedTransactionFailure:
            pass
        else:
            raise AssertionError("pre-persist injection did not fire")
        assert not path.exists()
    print("shared GUI transaction primitive tests passed")


if __name__ == "__main__":
    main()
