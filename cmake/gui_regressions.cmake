# SPDX-License-Identifier: GPL-3.0-or-later
if(BUILD_TESTING)
    find_program(LD_PYTHON3_EXECUTABLE NAMES python3 REQUIRED)

    add_test(
        NAME linux-defragger-gui-models
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_models.py")
    add_test(
        NAME linux-defragger-gui-services
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_services.py")
    add_test(
        NAME linux-defragger-gui-volume-identity
        COMMAND "${LD_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gui_volume_identity.py")

    set_tests_properties(
        linux-defragger-gui-models
        linux-defragger-gui-services
        linux-defragger-gui-volume-identity
        PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui")
endif()
