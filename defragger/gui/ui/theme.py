# SPDX-License-Identifier: GPL-3.0-or-later
"""Canonical Mercedes-Benz inspired GTK3 theme shared by Linux Defragger windows."""

from __future__ import annotations

import subprocess
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gtk

_FONT = Path("/usr/share/fonts/truetype/linux-defragger/mb_corpo_s_regular.ttf")
_applied = False
_provider: Gtk.CssProvider | None = None


def _mb_family() -> str:
    if _FONT.is_file():
        try:
            result = subprocess.run(
                ["fc-scan", "--format=%{family[0]}", str(_FONT)],
                check=True,
                capture_output=True,
                text=True,
                timeout=2,
            )
            family = result.stdout.strip()
            if family:
                return family.replace("\\", "\\\\").replace('"', '\\"')
        except (OSError, subprocess.SubprocessError):
            pass
    return "Sans"


def apply_mb_theme() -> None:
    """Apply the project-wide MB black/silver theme once per process."""
    global _applied, _provider
    if _applied:
        return
    screen = Gdk.Screen.get_default()
    if screen is None:
        return
    family = _mb_family()
    css = f"""
    * {{
        font-family: \"{family}\", Sans;
        color: #f1f2f3;
    }}
    window, dialog, .background {{ background-color: #090a0b; }}
    headerbar, .titlebar {{
        background-image: none;
        background-color: #111315;
        border-bottom: 1px solid #777d82;
        color: #f5f6f7;
    }}
    headerbar label, .titlebar label {{ font-weight: bold; }}
    button {{
        background-image: none;
        background-color: #1d2023;
        color: #f2f3f4;
        border: 1px solid #7e858a;
        border-radius: 3px;
        padding: 6px 12px;
        box-shadow: none;
    }}
    button:hover {{ background-color: #2a2e32; border-color: #c3c7ca; }}
    button:active, button:checked {{ background-color: #383d42; }}
    button:disabled {{ color: #6d7276; border-color: #3d4144; background-color: #151719; }}
    entry, combobox button, spinbutton, textview, treeview, viewport, scrolledwindow {{
        background-color: #111315;
        color: #eef0f1;
        border-color: #4f5559;
    }}
    entry:selected, textview text selection, treeview.view:selected {{
        background-color: #60666b;
        color: #ffffff;
    }}
    treeview.view header button {{
        background-color: #1b1e20;
        border-color: #555b60;
        font-weight: bold;
    }}
    notebook > header {{ background-color: #0d0f10; border-color: #4f5559; }}
    notebook tab {{ background-color: #141719; padding: 7px 12px; }}
    notebook tab:checked {{ background-color: #292d30; }}
    progressbar trough {{ background-color: #17191b; border: 1px solid #555b60; }}
    progressbar progress {{ background-color: #b5b9bc; }}
    progressbar text {{ color: #f8f8f8; }}
    scrollbar slider {{ background-color: #777d82; border-radius: 3px; }}
    scrollbar slider:hover {{ background-color: #a9adb0; }}
    separator {{ background-color: #4b5054; }}
    tooltip {{ background-color: #202326; color: #f4f4f4; border: 1px solid #777d82; }}
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css.encode("utf-8"))
    Gtk.StyleContext.add_provider_for_screen(
        screen,
        provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
    )
    _provider = provider
    _applied = True
