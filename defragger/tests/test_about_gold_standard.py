#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-contract regression for the shared LINK-style About surface."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ABOUT = (ROOT / "gui" / "ui" / "about.py").read_text()
WINDOW = (ROOT / "gui" / "ui" / "window.py").read_text()

for required in (
    "class AboutInfo:",
    "class LinkStandardWindowView(WindowView):",
    "Gtk.AboutDialog(",
    'add_class("link-about-dialog")',
    "dialog.set_default_size(560, 520)",
    "dialog.set_size_request(520, 480)",
    "dialog.set_authors(list(info.authors))",
    "dialog.set_license(info.license_text)",
    "dialog.set_wrap_license(True)",
    'website_label="Project website"',
    "APP_ICON_NAME",
    'subtitle="DEFRAGMENTER · NATIVE FILESYSTEM OPTIMISATION"',
    '"Shannon Smith — Author and project maintainer"',
):
    assert required in ABOUT, required

assert "from .about import LinkStandardWindowView" in WINDOW
assert "self.view = LinkStandardWindowView(" in WINDOW
assert "self.view = WindowView(" not in WINDOW

print("LINK-standard About presentation contract passed")
