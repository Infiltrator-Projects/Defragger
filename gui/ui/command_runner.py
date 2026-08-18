# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK-neutral lifecycle for one ordinary or privileged command.

``CommandRunner`` owns ordinary subprocesses and the one-command-at-a-time
state machine.  The reusable administrator transport lives in
``PrivilegeSession`` and communicates only through typed events/completions.
"""

from __future__ import annotations

import os
import signal
import subprocess
import threading

from core.protocol import EngineEventParser

from .command_models import (
    CommandCompletion,
    CommandRequest,
    EventCallback,
    RunnerEvent,
    Scheduler,
)
from .privilege_session import PrivilegeSession

# Preserve the public import surface used by the window and third-party tests.
__all__ = [
    "CommandCompletion",
    "CommandRequest",
    "CommandRunner",
    "RunnerEvent",
]


class CommandRunner:
    """Coordinate one command without owning administrator IPC details."""

    def __init__(
        self,
        *,
        mapper: str,
        operation_engine: str,
        privileged_helper: str,
        on_event: EventCallback,
        scheduler: Scheduler,
    ) -> None:
        self._on_event = on_event
        self._scheduler = scheduler
        self.busy = False
        self.stop_requested = False
        self.process: subprocess.Popen[str] | None = None
        self._active_request: CommandRequest | None = None
        self._privilege = PrivilegeSession(
            mapper=mapper,
            operation_engine=operation_engine,
            privileged_helper=privileged_helper,
            on_event=self._on_privilege_event,
            on_complete=self._privileged_finished,
            scheduler=scheduler,
        )

    @property
    def active_purpose(self) -> str:
        request = self._active_request
        return request.purpose if request is not None else ""

    @property
    def helper_ready(self) -> bool:
        return self._privilege.ready

    @property
    def helper_starting(self) -> bool:
        return self._privilege.starting

    def _deliver_event(self, event: RunnerEvent) -> bool:
        self._on_event(event)
        return False

    def _emit(self, event: RunnerEvent, *, from_worker: bool = False) -> None:
        if from_worker:
            self._scheduler(self._deliver_event, event)
        else:
            self._deliver_event(event)

    def _on_privilege_event(self, event: RunnerEvent) -> None:
        if event.kind == "stop-failed":
            self.stop_requested = False
        self._emit(event)

    def authenticate(self) -> None:
        """Start administrator authentication before the first operation."""

        self._privilege.authenticate(self.active_purpose)

    def run(self, request: CommandRequest) -> bool:
        """Start one command; return false while another command is active."""

        if self.busy:
            return False
        if not request.arguments:
            raise ValueError("command request has no executable")
        self.busy = True
        self.stop_requested = False
        self._active_request = request
        self._emit(RunnerEvent("started", request.purpose))
        if request.privileged:
            self._privilege.run(request)
        else:
            self._start_local_operation(request)
        return True

    def _start_local_operation(self, request: CommandRequest) -> None:
        def worker() -> None:
            output_parts: list[str] = []
            try:
                process = subprocess.Popen(
                    list(request.arguments),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    start_new_session=True,
                    env={**os.environ, "LC_ALL": "C", "LANG": "C"},
                )
                self.process = process
                assert process.stdout is not None
                for line in process.stdout:
                    clean = line.rstrip("\n")
                    if EngineEventParser.is_event_line(clean):
                        self._emit(
                            RunnerEvent("engine", request.purpose, clean),
                            from_worker=True,
                        )
                        continue
                    output_parts.append(line)
                    if request.stream_output:
                        self._emit(
                            RunnerEvent("output", request.purpose, clean),
                            from_worker=True,
                        )
                returncode = process.wait()
            except Exception as exc:
                returncode = 127
                output_parts.append(str(exc))
            finally:
                self.process = None
            self._scheduler(
                self._finish,
                CommandCompletion(
                    returncode,
                    "".join(output_parts),
                    request.purpose,
                ),
            )

        threading.Thread(target=worker, daemon=True).start()

    def _privileged_finished(self, completion: CommandCompletion) -> None:
        self._finish(completion)

    def _finish(self, completion: CommandCompletion) -> bool:
        request = self._active_request
        if request is None:
            return False
        self.busy = False
        self.stop_requested = False
        self.process = None
        self._active_request = None
        request.on_complete(completion)
        return False

    def request_stop(self) -> bool:
        """Request safe Stop for the active local or privileged process."""

        request = self._active_request
        if request is None or not self.busy or self.stop_requested:
            return False
        self.stop_requested = True
        purpose = request.purpose
        self._emit(
            RunnerEvent(
                "stop-requested",
                purpose,
                "Stop requested. Waiting for the engine to reach the next safe "
                "transaction boundary…",
            )
        )
        if request.privileged:
            delivered = self._privilege.request_stop(purpose)
            if not delivered:
                self.stop_requested = False
            return delivered

        process = self.process
        if process is None:
            self.stop_requested = False
            self._emit(
                RunnerEvent(
                    "stop-failed",
                    purpose,
                    "The engine process was not available for a Stop request.",
                )
            )
            return False
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGINT)
            self._emit(
                RunnerEvent(
                    "stop-delivered",
                    purpose,
                    "SIGINT delivered to the engine process group.",
                )
            )
            return True
        except ProcessLookupError:
            self._emit(
                RunnerEvent(
                    "stop-delivered",
                    purpose,
                    "The engine process has already exited.",
                )
            )
            return True
        except PermissionError as exc:
            self.stop_requested = False
            self._emit(
                RunnerEvent(
                    "stop-failed",
                    purpose,
                    f"Unable to signal process group: {exc}",
                )
            )
            return False

    def shutdown(self) -> None:
        """Close the reusable administrator session safely."""

        self._privilege.shutdown()
