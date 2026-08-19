#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small compatibility launcher for the modular GTK application."""

from ui.application import LinuxDefraggerApplication, main
from ui.window import MainWindow

__all__ = ["LinuxDefraggerApplication", "MainWindow", "main"]

if __name__ == "__main__":
    raise SystemExit(main())
