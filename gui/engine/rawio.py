# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact raw-device I/O shared by analysers and filesystem writers."""

from __future__ import annotations

import fcntl
import os
import stat
import struct
from pathlib import Path

from core.devices import require_unmounted

BLKGETSIZE64 = 0x80081272


def device_size(fd: int) -> int:
    """Return the byte size of an image or block device descriptor."""

    info = os.fstat(fd)
    if stat.S_ISREG(info.st_mode):
        return int(info.st_size)
    if stat.S_ISBLK(info.st_mode):
        try:
            raw = fcntl.ioctl(fd, BLKGETSIZE64, b"\0" * 8)
            return int(struct.unpack("=Q", raw)[0])
        except OSError:
            return int(os.lseek(fd, 0, os.SEEK_END))
    return int(info.st_size)


def read_exact_fd(fd: int, length: int, offset: int) -> bytes:
    """Read exactly *length* bytes with retry-safe positional I/O."""

    if offset < 0 or length < 0:
        raise ValueError("negative raw read")
    output = bytearray()
    while len(output) < length:
        chunk = os.pread(fd, length - len(output), offset + len(output))
        if not chunk:
            raise EOFError(
                f"short raw read: wanted {length} bytes at {offset}, got {len(output)}"
            )
        output.extend(chunk)
    return bytes(output)


def write_exact_fd(fd: int, data: bytes | bytearray | memoryview, offset: int) -> None:
    """Write the complete buffer with positional I/O."""

    if offset < 0:
        raise ValueError("negative raw write")
    view = memoryview(data)
    written = 0
    while written < len(view):
        count = os.pwrite(fd, view[written:], offset + written)
        if count <= 0:
            raise OSError(f"short raw write at byte {offset + written}")
        written += count


class RawDevice:
    """Exact-I/O wrapper around an unmounted block device or image."""

    def __init__(self, path: str | Path, *, writable: bool = False):
        self.path = os.path.realpath(str(path))
        info = os.stat(self.path)
        if not (stat.S_ISREG(info.st_mode) or stat.S_ISBLK(info.st_mode)):
            raise OSError("raw target must be a block device or regular image")
        if writable:
            require_unmounted(self.path)
        flags = os.O_RDWR if writable else os.O_RDONLY
        if writable and stat.S_ISBLK(info.st_mode):
            flags |= getattr(os, "O_EXCL", 0)
        self.fd = os.open(self.path, flags | getattr(os, "O_CLOEXEC", 0))
        self.size = device_size(self.fd)

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self) -> "RawDevice":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def read_exact(self, length: int, offset: int) -> bytes:
        return read_exact_fd(self.fd, length, offset)

    def write_exact(self, data: bytes | bytearray | memoryview, offset: int) -> None:
        write_exact_fd(self.fd, data, offset)

    def sync(self) -> None:
        os.fsync(self.fd)
