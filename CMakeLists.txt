# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

# Keep the established project/build graph isolated from filesystem-specific
# native modules so new C engines can be added without duplicating the registry.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/project.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ufs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/zfs.cmake")
