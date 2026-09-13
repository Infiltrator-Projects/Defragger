#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""GUI-first release updater that runs before the installed Defragger window."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from version import BUILD_PROFILE, VERSION
from ui.update_service import (
    UpdateError,
    check_latest,
    cleanup_download,
    download_update,
    install_update,
)

CACHE_MAX_AGE = 6 * 60 * 60


def _cache_path() -> Path:
    base = Path(os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache"))
    return base / "linux-defragger" / "update-check.json"


def _recently_checked() -> bool:
    path = _cache_path()
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
        return (
            payload.get("version") == VERSION
            and time.time() - float(payload.get("checked_at", 0)) < CACHE_MAX_AGE
        )
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return False


def _remember_check() -> None:
    path = _cache_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_suffix(".tmp")
        temp.write_text(
            json.dumps({"version": VERSION, "checked_at": time.time()}),
            encoding="utf-8",
        )
        os.replace(temp, path)
    except OSError:
        pass


def _load_gtk():
    try:
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        from gi.repository import Gdk, GLib, Gtk
    except (ImportError, ValueError):
        return None
    if not Gtk.init_check()[0]:
        return None

    css = b"""
    * {
        font-family: "MB Corpo S Title WEB", "DejaVu Sans", sans-serif;
    }
    dialog, messagedialog {
        background-color: #0b0d0f;
        color: #e7e9eb;
    }
    label {
        color: #e7e9eb;
    }
    button {
        background-image: none;
        background-color: #171a1d;
        color: #e7e9eb;
        border: 1px solid #555c62;
        border-radius: 4px;
        padding: 7px 14px;
    }
    button:hover {
        background-color: #262a2e;
        border-color: #a8afb5;
    }
    button.suggested-action {
        background-color: #c8ccd0;
        color: #090a0b;
        border-color: #eceeef;
        font-weight: bold;
    }
    progressbar trough {
        min-height: 12px;
        background-color: #15181a;
    }
    progressbar progress {
        background-color: #b9bec2;
    }
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css)
    screen = Gdk.Screen.get_default()
    if screen is not None:
        Gtk.StyleContext.add_provider_for_screen(
            screen,
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )
    return Gtk, GLib


def _message(Gtk, title: str, message: str, *, error: bool = False) -> None:
    dialog = Gtk.MessageDialog(
        modal=True,
        message_type=Gtk.MessageType.ERROR if error else Gtk.MessageType.INFO,
        buttons=Gtk.ButtonsType.CLOSE,
        text=title,
    )
    dialog.format_secondary_text(message)
    dialog.run()
    dialog.destroy()


def _offer(Gtk, version: str) -> bool:
    dialog = Gtk.MessageDialog(
        modal=True,
        message_type=Gtk.MessageType.INFO,
        buttons=Gtk.ButtonsType.CANCEL,
        text=f"Linux Defragger {version} is available",
    )
    profile = (
        "The hardware-optimised local build will be preserved."
        if BUILD_PROFILE == "native"
        else "The verified AMD64 Debian package will be installed."
    )
    dialog.format_secondary_text(
        f"You are running {VERSION}. {profile}\n\n"
        "The release package and SHA-256 manifest will be downloaded from the "
        "official Infiltrator-Projects GitHub release, verified, and then "
        "installed using the normal graphical administrator-authentication prompt."
    )
    button = dialog.add_button("Install update", Gtk.ResponseType.OK)
    button.get_style_context().add_class("suggested-action")
    response = dialog.run()
    dialog.destroy()
    return response == Gtk.ResponseType.OK


def _install_with_progress(Gtk, GLib, release) -> None:
    dialog = Gtk.Dialog(title=f"Updating Linux Defragger to {release.version}", modal=True)
    dialog.set_default_size(470, 150)
    content = dialog.get_content_area()
    content.set_border_width(20)
    content.set_spacing(12)
    label = Gtk.Label(label="Downloading and verifying the published release…")
    label.set_xalign(0)
    label.set_line_wrap(True)
    content.pack_start(label, False, False, 0)
    spinner = Gtk.Spinner()
    spinner.start()
    content.pack_start(spinner, False, False, 0)
    dialog.show_all()

    outcome = {"path": None, "error": None}

    def worker() -> None:
        try:
            package_path, expected_sha256 = download_update(release, VERSION)
            outcome["path"] = package_path
            GLib.idle_add(
                label.set_text,
                "Release verified. Waiting for administrator authentication "
                "and installing the update…",
            )
            install_update(release, package_path, expected_sha256)
        except Exception as exc:
            outcome["error"] = exc
        finally:
            GLib.idle_add(dialog.response, Gtk.ResponseType.OK)

    threading.Thread(target=worker, daemon=True).start()
    dialog.run()
    dialog.destroy()
    spinner.stop()

    package_path = outcome["path"]
    cleanup_download(package_path)
    if outcome["error"] is not None:
        raise outcome["error"]


def _launch_main(arguments: list[str]) -> None:
    gui = "/usr/lib/linux-defragger/linux_defragger_gui.py"
    os.execv("/usr/bin/python3", ["/usr/bin/python3", gui, *arguments])


def main() -> int:
    manual = False
    passthrough: list[str] = []
    for argument in sys.argv[1:]:
        if argument == "--check-updates":
            manual = True
        else:
            passthrough.append(argument)

    gtk = _load_gtk()
    if gtk is None:
        if manual:
            print(
                "Linux Defragger update check requires a graphical GTK session.",
                file=sys.stderr,
            )
            return 1
        _launch_main(passthrough)
        return 0

    Gtk, GLib = gtk
    if not manual and _recently_checked():
        _launch_main(passthrough)
        return 0

    try:
        release = check_latest(VERSION, BUILD_PROFILE)
        _remember_check()
    except UpdateError as exc:
        if manual:
            _message(Gtk, "Unable to check for updates", str(exc), error=True)
            return 1
        _launch_main(passthrough)
        return 0

    if release is None:
        if manual:
            _message(
                Gtk,
                "Linux Defragger is up to date",
                f"Version {VERSION} is the latest published release.",
            )
            return 0
        _launch_main(passthrough)
        return 0

    if _offer(Gtk, release.version):
        try:
            _install_with_progress(Gtk, GLib, release)
        except UpdateError as exc:
            _message(Gtk, "Update was not installed", str(exc), error=True)
            if manual:
                return 1
        else:
            _message(
                Gtk,
                "Linux Defragger updated",
                f"Version {release.version} was verified and installed successfully.",
            )

    if manual:
        return 0
    _launch_main(passthrough)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
