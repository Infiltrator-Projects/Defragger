# SPDX-License-Identifier: GPL-3.0-or-later
# Focused integration coverage for the shared FAT12/FAT16/FAT32 relayout engine.

if(BUILD_TESTING AND TARGET linux-defragger-fat-worker)
    add_test(
        NAME linux-defragger-fat-relayout
        COMMAND bash
                "${CMAKE_CURRENT_LIST_DIR}/../tests/test_fat_relayout_engine.sh"
                "$<TARGET_FILE:linux-defragger-fat-worker>"
    )
    set_tests_properties(
        linux-defragger-fat-relayout
        PROPERTIES
            TIMEOUT 180
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1"
    )
endif()
