# SPDX-License-Identifier: GPL-3.0-or-later
"""Runtime theme policy for Linux Defragger.

Three explicit modes are supported:
- system: keep GTK/Cinnamon colours and apply only Infiltrator typography;
- day: force the light Infiltrator palette;
- night: force the graphite/silver Infiltrator palette.

The preference is per-user and shared by every Defragger window.
"""

from __future__ import annotations

from enum import Enum
from pathlib import Path
import os
import subprocess

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gtk

_FONT = Path("/usr/share/fonts/truetype/linux-defragger/mb_corpo_s_regular.ttf")
_CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "linux-defragger"
_CONFIG_FILE = _CONFIG_DIR / "theme"

_provider: Gtk.CssProvider | None = None


class ThemeMode(str, Enum):
    SYSTEM = "system"
    DAY = "day"
    NIGHT = "night"


def theme_label(mode: ThemeMode) -> str:
    return {
        ThemeMode.SYSTEM: "Follow system",
        ThemeMode.DAY: "Day",
        ThemeMode.NIGHT: "Night",
    }[mode]


def load_theme_mode() -> ThemeMode:
    try:
        value = _CONFIG_FILE.read_text(encoding="utf-8").strip()
        return ThemeMode(value)
    except (OSError, ValueError):
        return ThemeMode.SYSTEM


def save_theme_mode(mode: ThemeMode) -> None:
    try:
        _CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        _CONFIG_FILE.write_text(mode.value + "\n", encoding="utf-8")
    except OSError:
        # Appearance persistence must never stop a storage/safety tool starting.
        pass


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


def _base_css(family: str) -> str:
    return f"""
    * {{ font-family: "{family}", Sans; }}
    .app-title, .about-title {{
        font-family: "MB Corpo A Title Cond WEB", "{family}", Sans;
        font-weight: normal;
    }}
    .app-title {{ font-size: 23pt; }}
    .app-subtitle {{ font-size: 9.5pt; }}
    .summary-title {{ font-size: 8.75pt; }}
    .summary-value {{ font-size: 15pt; font-weight: bold; }}
    .section-title {{ font-size: 9.5pt; font-weight: bold; padding: 0 5px; }}
    button {{ border-radius: 3px; padding: 7px 13px; min-height: 27px; }}
    button.primary-action, button.operation-action {{ font-weight: bold; }}
    progressbar trough {{ min-height: 8px; }}
    .status-prefix {{ font-size: 8pt; font-weight: bold; }}
    .status-text {{ font-size: 8.75pt; }}
    """


def _night_css() -> str:
    return """
    window, dialog, .background, .app-shell { background-color: #050608; color: #e8ecef; }
    headerbar, .titlebar { background-image: none; background-color: #101318; color: #eef1f3; border-bottom: 1px solid #353a40; }
    menubar, .app-menubar { background-color: #0d1014; border-bottom: 1px solid #353a40; }
    menu { background-color: #101318; border: 1px solid #454b50; }
    menuitem:hover { background-color: #2b3137; }
    .app-title, .about-title, .summary-value { color: #eef1f3; }
    .app-subtitle, .summary-title, .map-caption, .status-text { color: #899198; }
    .section-title, .legend-item label, .log-expander { color: #aeb6bd; }
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {
        background-color: #0d1014; border: 1px solid #353a40;
    }
    button, combobox button, entry, spinbutton {
        background-image: none; background-color: #171b20; color: #e8ecef;
        border: 1px solid #5f666c; box-shadow: none;
    }
    button:hover { background-color: #22272d; border-color: #9da3a8; }
    button:active, button:checked { background-color: #2b3137; border-color: #bec7cf; }
    button:disabled { color: #59636c; border-color: #2a2e31; background-color: #0e1115; }
    button.primary-action { background-color: #d7dde2; color: #111418; border-color: #eef1f3; }
    button.primary-action:hover { background-color: #eef1f3; color: #111418; }
    button.destructive-action { border-color: #8f5555; color: #d8c5c5; }
    button.destructive-action:hover { background-color: #4a2525; border-color: #c36a6a; }
    progressbar trough { background-color: #101318; border: 1px solid #353a40; }
    progressbar progress { background-color: #bec7cf; }
    textview, textview text, treeview, viewport, scrolledwindow {
        background-color: #0e1115; color: #e8ecef; border-color: #353a40;
    }
    textview.log-view, textview.log-view text { background-color: #090b0d; color: #d9dde0; }
    entry selection, textview text selection, treeview.view:selected { background-color: #2b3137; color: #eef1f3; }
    .status-strip { background-color: #0d1014; border-top: 1px solid #353a40; }
    scrollbar slider { background-color: #555d63; }
    tooltip { background-color: #171b20; color: #eef1f3; border: 1px solid #5d646a; }
    """


def _day_css() -> str:
    return """
    window, dialog, .background, .app-shell { background-color: #f4f5f7; color: #20252b; }
    headerbar, .titlebar { background-image: none; background-color: #ffffff; color: #111418; border-bottom: 1px solid #c7cdd3; }
    menubar, .app-menubar { background-color: #ffffff; border-bottom: 1px solid #c7cdd3; }
    menu { background-color: #ffffff; border: 1px solid #c7cdd3; }
    menuitem:hover { background-color: #eceff2; }
    .app-title, .about-title, .summary-value { color: #111418; }
    .app-subtitle, .summary-title, .map-caption, .status-text { color: #737d86; }
    .section-title, .legend-item label, .log-expander { color: #59636c; }
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {
        background-color: #ffffff; border: 1px solid #c7cdd3;
    }
    button, combobox button, entry, spinbutton {
        background-image: none; background-color: #f8f9fa; color: #20252b;
        border: 1px solid #aeb6bd; box-shadow: none;
    }
    button:hover { background-color: #eceff2; border-color: #6f7881; }
    button:active, button:checked { background-color: #dde2e7; border-color: #6f7881; }
    button:disabled { color: #9aa2a9; border-color: #d8dde2; background-color: #f4f5f7; }
    button.primary-action { background-color: #20252b; color: #ffffff; border-color: #20252b; }
    button.primary-action:hover { background-color: #343b42; color: #ffffff; }
    button.destructive-action { border-color: #b54848; color: #8f3636; }
    button.destructive-action:hover { background-color: #f7e5e5; border-color: #b54848; }
    progressbar trough { background-color: #eceff2; border: 1px solid #c7cdd3; }
    progressbar progress { background-color: #6f7881; }
    textview, textview text, treeview, viewport, scrolledwindow {
        background-color: #ffffff; color: #20252b; border-color: #c7cdd3;
    }
    textview.log-view, textview.log-view text { background-color: #ffffff; color: #20252b; }
    entry selection, textview text selection, treeview.view:selected { background-color: #dde2e7; color: #111418; }
    .status-strip { background-color: #ffffff; border-top: 1px solid #c7cdd3; }
    scrollbar slider { background-color: #aeb6bd; }
    tooltip { background-color: #ffffff; color: #20252b; border: 1px solid #aeb6bd; }
    """


def apply_theme(mode: ThemeMode | str | None = None) -> ThemeMode:
    """Apply one theme globally to GTK; system mode leaves OS colours intact."""
    global _provider

    resolved = load_theme_mode() if mode is None else ThemeMode(mode)
    screen = Gdk.Screen.get_default()
    if screen is None:
        return resolved

    family = _mb_family()
    css = _base_css(family)
    if resolved is ThemeMode.DAY:
        css += _day_css()
    elif resolved is ThemeMode.NIGHT:
        css += _night_css()

    if _provider is None:
        _provider = Gtk.CssProvider()
        Gtk.StyleContext.add_provider_for_screen(
            screen,
            _provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 50,
        )
    _provider.load_from_data(css.encode("utf-8"))
    return resolved
