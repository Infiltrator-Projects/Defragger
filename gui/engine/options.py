# SPDX-License-Identifier: GPL-3.0-or-later
"""Command-line option filtering shared by plugin workers."""

from __future__ import annotations


def without_options(arguments: list[str], unsupported: tuple[str, ...]) -> list[str]:
    """Return *arguments* without unsupported ``--option [value]`` entries."""

    if not unsupported:
        return list(arguments)
    blocked = set(unsupported)
    filtered: list[str] = []
    index = 0
    while index < len(arguments):
        token = arguments[index]
        if token in blocked:
            index += 1
            if index < len(arguments) and not arguments[index].startswith("--"):
                index += 1
            continue
        if any(token.startswith(option + "=") for option in blocked):
            index += 1
            continue
        filtered.append(token)
        index += 1
    return filtered
