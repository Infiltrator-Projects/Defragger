# SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility proxy for ``filesystems.minix.plugin``."""

from filesystems.minix import plugin as _plugin

BACKEND = _plugin.BACKEND

def __getattr__(name: str):
    return getattr(_plugin, name)

__all__ = ["BACKEND"]
