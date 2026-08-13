# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(linux_defragger VERSION 1.8.0 LANGUAGES C)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LINUX_DEFRAGGER_PACKAGE_VERSION)
string(STRIP "${LINUX_DEFRAGGER_PACKAGE_VERSION}" LINUX_DEFRAGGER_PACKAGE_VERSION)
if(NOT LINUX_DEFRAGGER_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a release such as 1.8.0-49")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
find_package(Threads REQUIRED)

option(LD_ENABLE_WERROR "Treat first-party compiler warnings as errors" OFF)
option(LD_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)
option(LD_GENERIC_AMD64 "Build for the baseline x86-64 instruction set" OFF)
option(LD_NATIVE_OPTIMIZATION "Optimise native code for this build machine" OFF)

if(LD_GENERIC_AMD64 AND LD_NATIVE_OPTIMIZATION)
    message(FATAL_ERROR
        "LD_GENERIC_AMD64 and LD_NATIVE_OPTIMIZATION are mutually exclusive")
endif()

# Release artifacts deliberately have two CPU policies.  The distributed
# amd64 package targets the original x86-64 baseline, while the local .run
# installer selects the compiler's complete native target for that machine.
include(CheckCCompilerFlag)
set(LD_CPU_FLAGS)
if(LD_GENERIC_AMD64)
    check_c_compiler_flag("-march=x86-64" LD_HAS_MARCH_X86_64)
    check_c_compiler_flag("-mtune=generic" LD_HAS_MTUNE_GENERIC)
    if(NOT LD_HAS_MARCH_X86_64 OR NOT LD_HAS_MTUNE_GENERIC)
        message(FATAL_ERROR
            "The selected compiler cannot produce the generic amd64 build")
    endif()
    list(APPEND LD_CPU_FLAGS -march=x86-64 -mtune=generic)
elseif(LD_NATIVE_OPTIMIZATION)
    check_c_compiler_flag("-march=native" LD_HAS_MARCH_NATIVE)
    check_c_compiler_flag("-mtune=native" LD_HAS_MTUNE_NATIVE)
    if(NOT LD_HAS_MARCH_NATIVE OR NOT LD_HAS_MTUNE_NATIVE)
        message(FATAL_ERROR
            "The selected compiler does not support native CPU optimisation")
    endif()
    list(APPEND LD_CPU_FLAGS -march=native -mtune=native)
endif()

if(LD_CPU_FLAGS)
    add_compile_options(${LD_CPU_FLAGS})
endif()

set(LD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LD_GENERATED_DIR}")
configure_file(src/core/version.h.in "${LD_GENERATED_DIR}/version.h" @ONLY)
configure_file(packaging/generated/version.py.in "${LD_GENERATED_DIR}/version.py" @ONLY)

set(LD_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
if(LD_ENABLE_WERROR)
    list(APPEND LD_WARNING_FLAGS -Werror)
endif()
if(LD_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(linux-defragger-core STATIC
    src/core/ld_runtime.c
    src/core/ld_io.c
    src/core/ld_device.c
    src/core/ld_stop.c
    src/core/ld_stage.c)
target_include_directories(linux-defragger-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-core PUBLIC _FILE_OFFSET_BITS=64 _GNU_SOURCE)

# FAT remains native C, but it is a private implementation detail of the
# authoritative gui/filesystems/fat plugin.  There is deliberately no second
# native filesystem registry or plugin ABI.
add_executable(linux-defragger-fat-worker
    gui/filesystems/fat/native/writer.c
    gui/filesystems/fat/native/fat_analysis.c
    gui/filesystems/fat/native/fat_directory.c
    gui/filesystems/fat/native/fat_growth.c
    gui/filesystems/fat/native/fat_io.c
    gui/filesystems/fat/native/fat_journal.c
    gui/filesystems/fat/native/fat_relocation.c
    gui/filesystems/fat/native/fat_volume.c)
target_include_directories(linux-defragger-fat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-fat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-fat-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-fat-worker PRIVATE
    linux-defragger-core Threads::Threads)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(linux-defragger-xfs-native STATIC
    gui/filesystems/xfs/native/xfs_common.c
    gui/filesystems/xfs/native/xfs_catalog.c
    gui/filesystems/xfs/native/xfs_plan.c
    gui/filesystems/xfs/native/xfs_metadata.c)
target_include_directories(linux-defragger-xfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-xfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-xfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-xfs-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

# EXT2/3/4 is implemented directly in native C through the linked libext2fs
# library.  Filesystem mutation is performed in-process by the plugin worker.
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXT2FS REQUIRED IMPORTED_TARGET ext2fs)
find_library(COM_ERR_LIBRARY com_err REQUIRED)

add_library(linux-defragger-ext-native STATIC
    gui/filesystems/ext4/native/ext_common.c
    gui/filesystems/ext4/native/ext_catalog.c
    gui/filesystems/ext4/native/ext_plan.c)
target_include_directories(linux-defragger-ext-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ext-native PRIVATE
    ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
target_compile_definitions(linux-defragger-ext-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ext-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

add_executable(linux-defragger-ext-worker
    gui/filesystems/ext4/native/ext_worker.c)
target_include_directories(linux-defragger-ext-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ext4/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ext-worker PRIVATE
    ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
target_compile_definitions(linux-defragger-ext-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ext-worker PRIVATE
    linux-defragger-ext-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

add_library(linux-defragger-ntfs-native STATIC
    gui/filesystems/ntfs/native/ntfs_common.c
    gui/filesystems/ntfs/native/ntfs_catalog.c
    gui/filesystems/ntfs/native/ntfs_plan.c)
target_include_directories(linux-defragger-ntfs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ntfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ntfs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ntfs-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ntfs-native PUBLIC
    linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

add_executable(linux-defragger-ntfs-worker
    gui/filesystems/ntfs/native/ntfs_worker.c)
target_include_directories(linux-defragger-ntfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/ntfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-ntfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-ntfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-ntfs-worker PRIVATE
    linux-defragger-ntfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

add_library(linux-defragger-exfat-native STATIC
    gui/filesystems/exfat/native/exfat_common.c
    gui/filesystems/exfat/native/exfat_plan.c)
target_include_directories(linux-defragger-exfat-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-exfat-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-exfat-native PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-exfat-native PUBLIC
    linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-exfat-worker
    gui/filesystems/exfat/native/exfat_worker.c)
target_include_directories(linux-defragger-exfat-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/exfat/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-exfat-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-exfat-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-exfat-worker PRIVATE
    linux-defragger-exfat-native linux-defragger-core OpenSSL::Crypto)


add_library(linux-defragger-affs-native STATIC
    gui/filesystems/affs/native/affs_native.c)
target_include_directories(linux-defragger-affs-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-affs-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-affs-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-affs-native PUBLIC linux-defragger-core)

add_executable(linux-defragger-affs-worker
    gui/filesystems/affs/native/affs_worker.c)
target_include_directories(linux-defragger-affs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/affs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-affs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-affs-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-affs-worker PRIVATE linux-defragger-affs-native linux-defragger-core)
add_library(linux-defragger-hfsplus-native STATIC
    gui/filesystems/hfsplus/native/hfsplus_native.c)
target_include_directories(linux-defragger-hfsplus-native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/hfsplus/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-hfsplus-native PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfsplus-native PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-hfsplus-native PUBLIC linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-hfsplus-worker
    gui/filesystems/hfsplus/native/hfsplus_worker.c)
target_include_directories(linux-defragger-hfsplus-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/hfsplus/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-hfsplus-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfsplus-worker PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-hfsplus-worker PRIVATE linux-defragger-hfsplus-native linux-defragger-core OpenSSL::Crypto)

add_executable(linux-defragger-xfs-worker
    gui/filesystems/xfs/native/xfs_worker.c)
target_include_directories(linux-defragger-xfs-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-xfs-worker PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-xfs-worker PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-xfs-worker PRIVATE
    linux-defragger-xfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)

# Classic HFS read-only analysis is first-party native C.  The analyser parses
# the MDB, Extents Overflow B-tree and Catalog B-tree directly; no bundled HFS
# library or external filesystem utility participates in the production path.
add_executable(linux-defragger-hfs-analyser gui/filesystems/hfs/native/analyser.c)
target_compile_options(linux-defragger-hfs-analyser PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-hfs-analyser PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
set_target_properties(linux-defragger-hfs-analyser PROPERTIES OUTPUT_NAME hfs_analyser)

install(TARGETS linux-defragger-affs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/affs)
install(TARGETS linux-defragger-fat-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/fat)
install(TARGETS linux-defragger-xfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/xfs)
install(TARGETS linux-defragger-ext-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ext4)
install(TARGETS linux-defragger-ntfs-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/ntfs)
install(TARGETS linux-defragger-exfat-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/exfat)
install(TARGETS linux-defragger-hfsplus-worker
        RUNTIME DESTINATION lib/linux-defragger/filesystems/hfsplus)
install(TARGETS linux-defragger-hfs-analyser
        RUNTIME DESTINATION lib/linux-defragger/filesystems/hfs)

install(PROGRAMS
    gui/linux_defragger_gui.py
    gui/allocation_mapper.py
    gui/privileged_helper.py
    gui/operation_engine.py
    DESTINATION lib/linux-defragger)
install(FILES "${LD_GENERATED_DIR}/version.py" DESTINATION lib/linux-defragger)

install(DIRECTORY gui/core gui/engine gui/ui gui/backends gui/filesystems
        DESTINATION lib/linux-defragger
        FILES_MATCHING PATTERN "*.py"
        PATTERN "__pycache__" EXCLUDE)

install(PROGRAMS packaging/linux-defragger DESTINATION bin)
install(PROGRAMS tools/linux-defragger-testdata.py
        DESTINATION bin RENAME linux-defragger-testdata)
install(FILES packaging/io.github.linuxdefragger.desktop
        DESTINATION share/applications)
install(FILES packaging/io.github.linuxdefragger.svg
        DESTINATION share/icons/hicolor/scalable/apps)


install(FILES README.md RELEASE_NOTES.md TEST_STATUS.md
              docs/FILESYSTEM_PLUGIN_CONTRACT.md docs/DESIGN.md docs/COPYRIGHT.md
        DESTINATION share/doc/linux-defragger)
install(FILES LICENSES/GPL-3.0-or-later.txt
        DESTINATION share/doc/linux-defragger RENAME COPYING.GPL-3.0)
include(CTest)
if(BUILD_TESTING)
    add_executable(linux-defragger-xfs-metadata-test
        tests/test_xfs_metadata_whitebox.c
        gui/filesystems/xfs/native/xfs_common.c)
    target_include_directories(linux-defragger-xfs-metadata-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-xfs-metadata-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-xfs-metadata-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-xfs-metadata-test PRIVATE
        linux-defragger-core SQLite::SQLite3)
    add_test(NAME linux-defragger-xfs-metadata COMMAND linux-defragger-xfs-metadata-test)

    add_executable(linux-defragger-xfs-native-test tests/test_xfs_native.c)
    target_include_directories(linux-defragger-xfs-native-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/xfs/native"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-xfs-native-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-xfs-native-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-xfs-native-test PRIVATE
        linux-defragger-xfs-native linux-defragger-core SQLite::SQLite3 OpenSSL::Crypto)
    add_test(NAME linux-defragger-xfs-native COMMAND linux-defragger-xfs-native-test)

    add_executable(linux-defragger-ext-fixture tests/make_ext4_image.c)
    target_compile_options(linux-defragger-ext-fixture PRIVATE
        ${LD_WARNING_FLAGS} -Wno-deprecated-declarations)
    target_link_libraries(linux-defragger-ext-fixture PRIVATE
        PkgConfig::EXT2FS ${COM_ERR_LIBRARY})

    add_executable(linux-defragger-native-core-test tests/test_native_core.c)
    target_include_directories(linux-defragger-native-core-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-native-core-test PRIVATE ${LD_WARNING_FLAGS})
    target_link_libraries(linux-defragger-native-core-test PRIVATE linux-defragger-core)
    add_test(NAME linux-defragger-native-core COMMAND linux-defragger-native-core-test)
    add_executable(linux-defragger-fat-volume-test
        tests/test_fat_volume.c
        gui/filesystems/fat/native/fat_io.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-volume-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-volume-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-volume-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-volume-test PRIVATE
        linux-defragger-core Threads::Threads)
    add_test(NAME linux-defragger-fat-volume
        COMMAND linux-defragger-fat-volume-test)
    add_executable(linux-defragger-fat-directory-test
        tests/test_fat_directory.c
        gui/filesystems/fat/native/fat_directory.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-directory-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-directory-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-directory-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-directory-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-directory
        COMMAND linux-defragger-fat-directory-test)
    add_executable(linux-defragger-fat-journal-test
        tests/test_fat_journal.c
        gui/filesystems/fat/native/fat_journal.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-journal-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-journal-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-journal-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-journal-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-journal
        COMMAND linux-defragger-fat-journal-test)
    add_executable(linux-defragger-fat-growth-test
        tests/test_fat_growth.c
        gui/filesystems/fat/native/fat_directory.c
        gui/filesystems/fat/native/fat_growth.c
        gui/filesystems/fat/native/fat_volume.c)
    target_include_directories(linux-defragger-fat-growth-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/filesystems/fat/native"
        "${LD_GENERATED_DIR}")
    target_compile_options(linux-defragger-fat-growth-test PRIVATE
        ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-fat-growth-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-fat-growth-test PRIVATE
        linux-defragger-core)
    add_test(NAME linux-defragger-fat-growth
        COMMAND linux-defragger-fat-growth-test)
    add_test(NAME linux-defragger-tests
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
                     $<TARGET_FILE:linux-defragger-fat-worker>)
endif()
