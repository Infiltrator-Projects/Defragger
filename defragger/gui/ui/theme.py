# SPDX-License-Identifier: GPL-3.0-or-later
"""Runtime theme policy for Linux Defragger.

Common owns semantic Day/Night palette values. This module owns only GTK
selector mechanics, user preference persistence and the platform-authoritative
Follow system mode.
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

from .theme_tokens import DAY, NIGHT

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
    p = NIGHT
    return f"""
    window, dialog, .background, .app-shell {{ background-color: {p["background"]}; color: {p["text"]}; }}
    headerbar, .titlebar {{ background-image: none; background-color: {p["panel"]}; color: {p["title"]}; border-bottom: 1px solid {p["border"]}; }}
    menubar, .app-menubar {{ background-color: {p["surface"]}; border-bottom: 1px solid {p["border"]}; }}
    menu {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    menuitem:hover {{ background-color: {p["operation_hover"]}; }}
    .app-title, .about-title, .summary-value {{ color: {p["title"]}; }}
    .app-subtitle, .summary-title, .map-caption, .status-text {{ color: {p["subtle"]}; }}
    .section-title, .legend-item label, .log-expander {{ color: {p["muted"]}; }}
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {{
        background-color: {p["surface"]}; border: 1px solid {p["border"]};
    }}
    button, combobox button, entry, spinbutton {{
        background-image: none; background-color: {p["card"]}; color: {p["text"]};
        border: 1px solid {p["neutral_accent"]}; box-shadow: none;
    }}
    button:hover {{ background-color: {p["card_hover"]}; border-color: {p["neutral_accent"]}; }}
    button:active, button:checked {{ background-color: {p["selection_background"]}; border-color: {p["neutral_accent"]}; }}
    button:disabled {{ color: {p["subtle"]}; border-color: {p["border"]}; background-color: {p["input"]}; }}
    button.primary-action {{ background-color: {p["button_background"]}; color: {p["button_foreground"]}; border-color: {p["button_background"]}; }}
    button.primary-action:hover {{ background-color: {p["equals_hover"]}; color: {p["button_foreground"]}; }}
    button.destructive-action {{ border-color: {p["fault"]}; color: {p["fault"]}; }}
    button.destructive-action:hover {{ background-color: {p["surface_hover"]}; border-color: {p["fault"]}; }}
    progressbar trough {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    progressbar progress {{ background-color: {p["neutral_accent"]}; }}
    textview, textview text, treeview, viewport, scrolledwindow {{
        background-color: {p["input"]}; color: {p["text"]}; border-color: {p["border"]};
    }}
    textview.log-view, textview.log-view text {{ background-color: {p["background"]}; color: {p["text"]}; }}
    entry selection, textview text selection, treeview.view:selected {{
        background-color: {p["selection_background"]}; color: {p["selection_foreground"]};
    }}
    .status-strip {{ background-color: {p["surface"]}; border-top: 1px solid {p["border"]}; }}
    scrollbar slider {{ background-color: {p["neutral_accent"]}; }}
    tooltip {{ background-color: {p["card"]}; color: {p["title"]}; border: 1px solid {p["border"]}; }}
    """


def _day_css() -> str:
    p = DAY
    return f"""
    window, dialog, .background, .app-shell {{ background-color: {p["background"]}; color: {p["text"]}; }}
    headerbar, .titlebar {{ background-image: none; background-color: {p["panel"]}; color: {p["title"]}; border-bottom: 1px solid {p["border"]}; }}
    menubar, .app-menubar {{ background-color: {p["panel"]}; border-bottom: 1px solid {p["border"]}; }}
    menu {{ background-color: {p["panel"]}; border: 1px solid {p["border"]}; }}
    menuitem:hover {{ background-color: {p["surface"]}; }}
    .app-title, .about-title, .summary-value {{ color: {p["title"]}; }}
    .app-subtitle, .summary-title, .map-caption, .status-text {{ color: {p["subtle"]}; }}
    .section-title, .legend-item label, .log-expander {{ color: {p["muted"]}; }}
    .version-badge, frame.section-panel > border, frame.map-panel > border,
    frame.action-panel > border, frame.summary-card > border {{
        background-color: {p["panel"]}; border: 1px solid {p["border"]};
    }}
    button, combobox button, entry, spinbutton {{
        background-image: none; background-color: {p["card"]}; color: {p["text"]};
        border: 1px solid {p["neutral_accent"]}; box-shadow: none;
    }}
    button:hover {{ background-color: {p["card_hover"]}; border-color: {p["neutral_accent"]}; }}
    button:active, button:checked {{ background-color: {p["selection_background"]}; border-color: {p["neutral_accent"]}; }}
    button:disabled {{ color: {p["subtle"]}; border-color: {p["border"]}; background-color: {p["background"]}; }}
    button.primary-action {{ background-color: {p["button_background"]}; color: {p["button_foreground"]}; border-color: {p["button_background"]}; }}
    button.primary-action:hover {{ background-color: {p["equals_hover"]}; color: {p["button_foreground"]}; }}
    button.destructive-action {{ border-color: {p["fault"]}; color: {p["fault"]}; }}
    button.destructive-action:hover {{ background-color: {p["surface_hover"]}; border-color: {p["fault"]}; }}
    progressbar trough {{ background-color: {p["surface"]}; border: 1px solid {p["border"]}; }}
    progressbar progress {{ background-color: {p["neutral_accent"]}; }}
    textview, textview text, treeview, viewport, scrolledwindow {{
        background-color: {p["panel"]}; color: {p["text"]}; border-color: {p["border"]};
    }}
    textview.log-view, textview.log-view text {{ background-color: {p["panel"]}; color: {p["text"]}; }}
    entry selection, textview text selection, treeview.view:selected {{
        background-color: {p["selection_background"]}; color: {p["selection_foreground"]};
    }}
    .status-strip {{ background-color: {p["panel"]}; border-top: 1px solid {p["border"]}; }}
    scrollbar slider {{ background-color: {p["neutral_accent"]}; }}
    tooltip {{ background-color: {p["panel"]}; color: {p["text"]}; border: 1px solid {p["border"]}; }}
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
