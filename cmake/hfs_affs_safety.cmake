# SPDX-License-Identifier: GPL-3.0-or-later

# HFS+ already links OpenSSL through its native library.  AFFS now persists a
# cryptographic digest for its verified recovery stage as well, so its worker
# needs the same host-supplied crypto provider.
target_link_libraries(linux-defragger-affs-worker PRIVATE OpenSSL::Crypto)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(
        NAME linux-defragger-affs-native-python
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_affs_native.py")
    set_tests_properties(linux-defragger-affs-native-python PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")

    add_test(
        NAME linux-defragger-hfsplus-native-python
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hfsplus_native.py")
    set_tests_properties(linux-defragger-hfsplus-native-python PROPERTIES
        ENVIRONMENT
            "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/gui:${CMAKE_CURRENT_SOURCE_DIR}/tests;LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endif()
