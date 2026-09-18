#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Defragmenter Python theme adapter from pinned Infiltratr Common.

Common is the palette source of truth. Defragmenter owns GTK selectors and
storage-tool-specific presentation states only.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / "shared/infiltratr-common/design/infiltrator-design-v1.json"
OUTPUT = ROOT / "gui/ui/theme_tokens.py"

KEYS = (
    "background", "panel", "card", "surface", "input", "border", "text",
    "title", "muted", "subtle", "button_background", "button_foreground",
    "selection_background", "selection_foreground", "neutral_accent",
    "success", "warning", "fault", "info", "operation", "card_hover",
    "surface_hover", "operation_hover", "equals_hover",
)


def render() -> str:
    data = json.loads(COMMON.read_text(encoding="utf-8"))
    theme = data["theme"]
    lines = [
        "# SPDX-License-Identifier: GPL-3.0-or-later",
        '"""Generated from Infiltratr Common design tokens. Do not edit."""',
        "",
        f"THEME_CONTRACT_VERSION = {int(theme['contract_version'])}",
        "",
    ]
    for name, mode in (("DAY", "day"), ("NIGHT", "night")):
        palette = theme["palettes"][mode]
        lines.append(f"{name} = {{")
        for key in KEYS:
            lines.append(f'    "{key}": "{palette[key]}",')
        lines.extend(["}", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    desired = render()
    actual = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
    if args.check:
        if actual != desired:
            raise SystemExit(
                "Defragmenter theme tokens are stale; run tools/update-theme-tokens.py"
            )
        return 0
    OUTPUT.write_text(desired, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
