# SPDX-License-Identifier: GPL-3.0-or-later
"""Typed, durable transaction journals shared by filesystem workers.

Filesystem engines still own their format-specific recovery payloads.  This
module owns the rules that must not drift between them: schema validation,
legal phase transitions, atomic replacement, durable removal and consistent
error translation.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Protocol

from .journal import fsync_directory, write_json_journal


class TransactionContractError(ValueError):
    """A journal does not satisfy its declared transaction contract."""


class InjectedTransactionFailure(RuntimeError):
    """Deterministic test-only interruption at a durable journal checkpoint."""


@dataclass(frozen=True, slots=True)
class TransactionSchema:
    """Validation contract for one filesystem's recovery journal."""

    filesystem: str
    marker_key: str
    marker_value: str
    schema_versions: frozenset[int]
    phase_key: str
    phases: frozenset[str]
    target_key: str = "device"
    schema_key: str = "schema"
    required_keys: frozenset[str] = frozenset()
    transitions: Mapping[str, frozenset[str]] | None = None

    def validate(self, payload: Mapping[str, Any]) -> "TransactionRecord":
        data = dict(payload)
        if data.get(self.marker_key) != self.marker_value:
            raise TransactionContractError(
                f"journal is not a {self.filesystem} transaction"
            )
        version = data.get(self.schema_key)
        if not isinstance(version, int) or version not in self.schema_versions:
            raise TransactionContractError(
                f"{self.filesystem} journal schema is unsupported"
            )
        phase = data.get(self.phase_key)
        if not isinstance(phase, str) or phase not in self.phases:
            raise TransactionContractError(
                f"{self.filesystem} journal phase is invalid"
            )
        target = data.get(self.target_key)
        if not isinstance(target, str) or not target:
            raise TransactionContractError(
                f"{self.filesystem} journal target is missing"
            )
        missing = sorted(key for key in self.required_keys if key not in data)
        if missing:
            raise TransactionContractError(
                f"{self.filesystem} journal is missing: {', '.join(missing)}"
            )
        return TransactionRecord(self, data)

    def validate_transition(self, previous: str, current: str) -> None:
        """Reject an impossible phase change while allowing checkpoints."""

        if previous == current or self.transitions is None:
            return
        allowed = self.transitions.get(previous, frozenset())
        if current not in allowed:
            raise TransactionContractError(
                f"{self.filesystem} transaction cannot move from "
                f"{previous!r} to {current!r}"
            )


@dataclass(slots=True)
class TransactionRecord:
    """Validated mutable state for one durable filesystem transaction."""

    schema: TransactionSchema
    payload: dict[str, Any]

    @property
    def phase(self) -> str:
        return str(self.payload[self.schema.phase_key])

    @property
    def target(self) -> str:
        return str(self.payload[self.schema.target_key])

    def transition(self, phase: str, **updates: Any) -> None:
        """Apply a validated phase change and related checkpoint fields."""

        if phase not in self.schema.phases:
            raise TransactionContractError(
                f"{self.schema.filesystem} transaction cannot enter {phase!r}"
            )
        self.schema.validate_transition(self.phase, phase)
        self.payload.update(updates)
        self.payload[self.schema.phase_key] = phase
        self.schema.validate(self.payload)


ErrorFactory = Callable[[str], Exception]


class TransactionCheckpoint(Protocol):
    def __call__(
        self,
        checkpoint: str,
        path: Path,
        record: TransactionRecord,
    ) -> None: ...


@dataclass(slots=True)
class TransactionFailureInjector:
    """Fail once at an explicitly selected persistence checkpoint.

    The injector is supplied by tests through dependency injection.  Production
    journals have no hook, environment variable or hidden command-line switch.
    """

    fail_at: str
    triggered: bool = False

    def __call__(
        self,
        checkpoint: str,
        _path: Path,
        _record: TransactionRecord,
    ) -> None:
        if not self.triggered and checkpoint == self.fail_at:
            self.triggered = True
            raise InjectedTransactionFailure(checkpoint)


class TransactionJournal:
    """Atomic persistence for records accepted by one ``TransactionSchema``."""

    def __init__(
        self,
        schema: TransactionSchema,
        *,
        error_factory: ErrorFactory = TransactionContractError,
        trailing_newline: bool = True,
        checkpoint: TransactionCheckpoint | None = None,
    ) -> None:
        self.schema = schema
        self._error_factory = error_factory
        self._trailing_newline = trailing_newline
        self._checkpoint = checkpoint

    def record(self, payload: Mapping[str, Any]) -> TransactionRecord:
        try:
            return self.schema.validate(payload)
        except TransactionContractError as exc:
            raise self._error_factory(str(exc)) from exc

    def load(self, path: str | Path) -> TransactionRecord:
        target = Path(path)
        try:
            payload = json.loads(target.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise self._error_factory(
                f"cannot read {self.schema.filesystem} recovery journal "
                f"{target}: {exc}"
            ) from exc
        if not isinstance(payload, dict):
            raise self._error_factory(
                f"{self.schema.filesystem} recovery journal is not an object"
            )
        return self.record(payload)

    def save(
        self,
        path: str | Path,
        record: TransactionRecord | Mapping[str, Any],
    ) -> TransactionRecord:
        checked = record if isinstance(record, TransactionRecord) else self.record(record)
        if checked.schema != self.schema:
            raise self._error_factory(
                "transaction record belongs to a different journal schema"
            )
        target = Path(path)
        if self._checkpoint is not None:
            self._checkpoint(f"before-persist:{checked.phase}", target, checked)
        try:
            if target.exists() and self.schema.transitions is not None:
                previous = self.load(target)
                self.schema.validate_transition(previous.phase, checked.phase)
            write_json_journal(
                target,
                checked.payload,
                trailing_newline=self._trailing_newline,
            )
        except (OSError, TransactionContractError) as exc:
            raise self._error_factory(
                f"cannot persist {self.schema.filesystem} recovery journal: {exc}"
            ) from exc
        if self._checkpoint is not None:
            self._checkpoint(f"after-persist:{checked.phase}", target, checked)
        return checked

    def transition(
        self,
        path: str | Path,
        record: TransactionRecord,
        phase: str,
        **updates: Any,
    ) -> None:
        try:
            record.transition(phase, **updates)
        except TransactionContractError as exc:
            raise self._error_factory(str(exc)) from exc
        self.save(path, record)

    def remove(self, path: str | Path) -> None:
        target = Path(path)
        try:
            target.unlink()
            fsync_directory(target.parent)
        except FileNotFoundError:
            return
        except OSError as exc:
            raise self._error_factory(
                f"cannot remove {self.schema.filesystem} recovery journal: {exc}"
            ) from exc


__all__ = [
    "TransactionContractError",
    "InjectedTransactionFailure",
    "TransactionCheckpoint",
    "TransactionFailureInjector",
    "TransactionJournal",
    "TransactionRecord",
    "TransactionSchema",
]
