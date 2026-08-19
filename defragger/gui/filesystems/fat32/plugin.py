# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Standard FAT analysis and direct raw layout rewriting.
#
# Comments describe design intent and non-obvious behaviour. They are kept
# concise so that the implementation remains readable and maintainable.

"""FAT32 backend declaration using the shared FAT implementation."""

from filesystems.fat.common import FatBackend


BACKEND = FatBackend(32)
