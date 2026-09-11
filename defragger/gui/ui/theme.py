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
        color: #edf0f2;
    }}

    window, dialog, .background, .app-shell {{
        background-color: #080a0b;
    }}

    headerbar, .titlebar {{
        background-image: none;
        background-color: #151719;
        border-bottom: 1px solid #34383c;
        color: #f5f6f7;
        box-shadow: none;
    }}
    headerbar label, .titlebar label {{
        color: #c8cdd1;
        font-weight: normal;
    }}

    menubar, .app-menubar {{
        background-color: #0d0f10;
        border-bottom: 1px solid #25292c;
        padding: 4px 8px;
    }}
    menubar menuitem {{
        color: #c7ccd0;
        padding: 5px 9px;
    }}
    menu {{
        background-color: #15181a;
        border: 1px solid #454b50;
    }}
    menuitem {{ padding: 7px 11px; }}
    menuitem:hover {{ background-color: #282c30; }}

    .app-title, .about-title {{
        font-family: \"MB Corpo A Title Cond WEB\", \"{family}\", Sans;
        color: #f6f7f8;
        font-weight: normal;
    }}
    .app-title {{ font-size: 23pt; }}
    .app-subtitle {{
        color: #939ba2;
        font-size: 9.5pt;
    }}

    .version-badge {{
        background-color: #0b0d0f;
        border: 1px solid #2f3438;
        border-left: 2px solid #aeb4b9;
        border-radius: 4px;
        padding: 5px 8px;
    }}
    .version-primary {{
        color: #d7dbde;
        font-size: 9pt;
        font-weight: bold;
    }}
    .version-secondary {{
        color: #727b82;
        font-size: 8.5pt;
    }}

    .section-title {{
        color: #aab1b7;
        font-size: 9.5pt;
        font-weight: bold;
        padding: 0 5px;
    }}
    frame.section-panel > border,
    frame.map-panel > border,
    frame.action-panel > border,
    frame.summary-card > border {{
        background-color: #0b0d0f;
        border: 1px solid #2c3135;
        border-radius: 5px;
        box-shadow: none;
    }}
    frame.summary-card > border {{
        background-color: #0c0f11;
        border-color: #31373c;
        border-top-color: #555c62;
    }}
    .summary-title {{
        color: #7f8991;
        font-size: 8.75pt;
        font-weight: normal;
    }}
    .summary-value {{
        color: #f4f5f6;
        font-size: 15pt;
        font-weight: bold;
    }}

    combobox button, entry, spinbutton {{
        background-image: none;
        background-color: #111416;
        color: #e8ebed;
        border: 1px solid #444a4f;
        border-radius: 3px;
        box-shadow: none;
        min-height: 28px;
    }}
    combobox button:hover, entry:focus, spinbutton:focus {{
        border-color: #7f878d;
        background-color: #15191c;
    }}

    button {{
        background-image: none;
        background-color: #191c1f;
        color: #dde1e4;
        border: 1px solid #5f666c;
        border-radius: 3px;
        padding: 7px 13px;
        min-height: 27px;
        box-shadow: none;
        text-shadow: none;
    }}
    button:hover {{
        background-color: #24282c;
        border-color: #9da3a8;
        color: #ffffff;
    }}
    button:active, button:checked {{
        background-color: #30353a;
        border-color: #b6bbc0;
    }}
    button:disabled {{
        color: #555d63;
        border-color: #2a2e31;
        background-color: #101214;
    }}

    button.primary-action {{
        background-color: #c3c8cc;
        color: #090a0b;
        border-color: #e4e7e9;
        font-weight: bold;
        min-width: 82px;
    }}
    button.primary-action:hover {{
        background-color: #e0e3e5;
        color: #050505;
    }}
    button.primary-action:active {{
        background-color: #aeb5ba;
        color: #050505;
    }}
    button.primary-action:disabled {{
        background-color: #171a1c;
        color: #596168;
        border-color: #30353a;
    }}
    button.operation-action {{
        border-color: #757d83;
        color: #e2e5e7;
        font-weight: bold;
        min-width: 96px;
    }}
    button.destructive-action {{
        border-color: #6c4545;
        color: #d8c5c5;
    }}
    button.destructive-action:hover {{
        background-color: #3a2222;
        border-color: #aa6262;
        color: #f3dddd;
    }}
    button.destructive-action:disabled {{
        color: #4f5356;
        border-color: #2a2e31;
        background-color: #101214;
    }}

    .legend-strip {{
        background-color: transparent;
        padding: 2px 0;
    }}
    .legend-item label {{
        color: #aab1b6;
        font-size: 8.75pt;
    }}
    .map-caption {{
        color: #707a82;
        font-size: 8.75pt;
    }}

    progressbar.operation-progress trough, progressbar trough {{
        min-height: 8px;
        background-color: #101315;
        border: 1px solid #30363a;
        border-radius: 2px;
    }}
    progressbar.operation-progress progress, progressbar progress {{
        background-color: #aeb4b9;
        border-radius: 1px;
    }}
    progressbar text {{
        color: #9ba2a8;
        font-size: 8pt;
    }}

    .log-expander {{
        color: #aeb4b9;
        font-size: 9.5pt;
        font-weight: bold;
    }}
    scrolledwindow.log-scroll {{
        background-color: #080a0b;
        border: 1px solid #2c3135;
        border-radius: 4px;
    }}
    textview.log-view,
    textview.log-view text {{
        background-color: #090b0d;
        color: #c9ced2;
        font-size: 9.25pt;
    }}
    textview.log-view text selection {{
        background-color: #555d63;
        color: #ffffff;
    }}

    .status-strip {{
        background-color: #0a0c0e;
        border-top: 1px solid #24292d;
        border-bottom: 1px solid #15181a;
    }}
    .status-prefix {{
        color: #5f6870;
        font-size: 8pt;
        font-weight: bold;
    }}
    .status-text {{
        color: #8c959c;
        font-size: 8.75pt;
    }}

    .about-title {{ font-size: 21pt; }}
    .about-version {{
        color: #8e979e;
        font-size: 9.5pt;
    }}
    .about-copy {{
        color: #c9ced2;
        font-size: 9.5pt;
    }}
    .about-meta-key {{
        color: #747e86;
        font-size: 8.75pt;
        font-weight: bold;
    }}
    .about-meta-value {{
        color: #c5cace;
        font-size: 9pt;
    }}
    linkbutton button {{
        background-color: transparent;
        border-color: #353b40;
        color: #c5cbd0;
        box-shadow: none;
    }}
    linkbutton button:hover {{
        background-color: #161a1d;
        border-color: #666e74;
    }}

    textview, textview text, treeview, viewport, scrolledwindow {{
        background-color: #0d1012;
        color: #dde1e4;
        border-color: #373d42;
    }}
    entry:selected, textview text selection, treeview.view:selected {{
        background-color: #555d63;
        color: #ffffff;
    }}
    treeview.view header button {{
        background-color: #171a1d;
        border-color: #41474c;
        font-weight: bold;
    }}
    notebook > header {{
        background-color: #0b0d0f;
        border-color: #343a3f;
    }}
    notebook tab {{
        background-color: #111416;
        padding: 7px 12px;
    }}
    notebook tab:checked {{ background-color: #24282c; }}

    scrollbar slider {{
        background-color: #555d63;
        border-radius: 3px;
        min-width: 7px;
        min-height: 7px;
    }}
    scrollbar slider:hover {{ background-color: #858c92; }}
    separator {{ background-color: #30353a; }}
    tooltip {{
        background-color: #1a1d20;
        color: #f1f2f3;
        border: 1px solid #5d646a;
    }}
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css.encode("utf-8"))
    Gtk.StyleContext.add_provider_for_screen(
        screen,
        provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 50,
    )
    _provider = provider
    _applied = True
