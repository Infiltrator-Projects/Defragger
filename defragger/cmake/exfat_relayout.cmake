# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    add_test(
        NAME linux-defragger-exfat-relayout
        COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../tests/test_exfat_relayout_engine.sh"
                "$<TARGET_FILE:linux-defragger-exfat-worker>")
    set_tests_properties(linux-defragger-exfat-relayout PROPERTIES
        ENVIRONMENT
            "LINUX_DEFRAGGER_ENABLE_UNAUDITED_WRITES=I_ACCEPT_UNAUDITED_RAW_WRITES")
endif()
