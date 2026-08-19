# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only binary helpers shared by filesystem analysers."""

from __future__ import annotations

import struct

from engine.rawio import RawDevice
from .contracts import BackendError

__all__ = ["Reader", "u16le", "u32le", "u64le", "u16be", "u32be", "u64be"]


class Reader:
    """Read-only view over the shared exact raw-device implementation."""

    def __init__(self, path: str):
        self._device = RawDevice(path, writable=False)
        self.path = path
        self.fd = self._device.fd
        self.size = self._device.size

    def close(self) -> None:
        self._device.close()
        self.fd = -1

    def read(self, offset: int, length: int) -> bytes:
        try:
            return self._device.read_exact(length, offset)
        except (EOFError, OSError, ValueError) as exc:
            raise BackendError(str(exc)) from exc

    def __enter__(self) -> "Reader":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def u16le(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32le(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64le(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def u16be(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def u32be(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def u64be(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]
