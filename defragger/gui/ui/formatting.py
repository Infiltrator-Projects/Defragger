# SPDX-License-Identifier: GPL-3.0-or-later
"""Small presentation-only formatting helpers."""

from __future__ import annotations


def human_bytes(value: int) -> str:
    # Traditional binary-sized labels: KB=1024 bytes, MB=1024 KB, and so on.
    units = ("B", "KB", "MB", "GB", "TB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} B"
        amount /= 1024.0
    return f"{value} B"
