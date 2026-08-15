# SPDX-License-Identifier: GPL-3.0-or-later
# Separate all-C GTK companion for creating and verifying sacrificial test media.

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_library(linux-defragger-test-media-core STATIC
    test_media/test_media_core.c
    test_media/test_media_worker.c)
target_include_directories(linux-defragger-test-media-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-test-media-core PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media-core PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-test-media-core PUBLIC OpenSSL::Crypto)

add_executable(linux-defragger-test-media
    test_media/test_media_main.c
    test_media/test_media_gui.c)
target_include_directories(linux-defragger-test-media PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_media"
    "${LD_GENERATED_DIR}")
target_compile_options(linux-defragger-test-media PRIVATE ${LD_WARNING_FLAGS})
target_compile_definitions(linux-defragger-test-media PRIVATE
    _FILE_OFFSET_BITS=64 _GNU_SOURCE)
target_link_libraries(linux-defragger-test-media PRIVATE
    linux-defragger-test-media-core PkgConfig::GTK3 OpenSSL::Crypto)

install(TARGETS linux-defragger-test-media RUNTIME DESTINATION bin)
install(FILES packaging/io.github.linuxdefragger.TestMedia.desktop
        DESTINATION share/applications)

if(BUILD_TESTING)
    add_executable(linux-defragger-test-media-test tests/test_test_media.c)
    target_include_directories(linux-defragger-test-media-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_media")
    target_compile_options(linux-defragger-test-media-test PRIVATE ${LD_WARNING_FLAGS})
    target_compile_definitions(linux-defragger-test-media-test PRIVATE
        _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_link_libraries(linux-defragger-test-media-test PRIVATE
        linux-defragger-test-media-core OpenSSL::Crypto)
    add_test(NAME linux-defragger-test-media-core COMMAND linux-defragger-test-media-test)
endif()
