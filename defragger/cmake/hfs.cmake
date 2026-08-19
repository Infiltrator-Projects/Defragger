# SPDX-License-Identifier: GPL-3.0-or-later

# The Classic HFS analyser target itself remains in project.cmake because it
# predates the per-filesystem CMake fragments.  This fragment permanently tests
# its exact native allocation-map interface.
if(BUILD_TESTING)
    add_executable(linux-defragger-hfs-native-test tests/test_hfs_worker.c)
    target_compile_options(linux-defragger-hfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-hfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    add_test(NAME linux-defragger-hfs-native
        COMMAND linux-defragger-hfs-native-test
                $<TARGET_FILE:linux-defragger-hfs-analyser>)
endif()
