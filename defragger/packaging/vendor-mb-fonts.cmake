# SPDX-License-Identifier: GPL-3.0-or-later
# Consume Common 1.19.8's canonical MB Corpo build-asset provenance.
if(NOT DEFINED LD_ROOT OR NOT DEFINED LD_FONT_ARCHIVE)
    message(FATAL_ERROR "LD_ROOT and LD_FONT_ARCHIVE are required")
endif()

include("${LD_ROOT}/shared/infiltratr-common/cmake/InfiltratrTypographyAssets.cmake")

get_filename_component(_archive_dir "${LD_FONT_ARCHIVE}" DIRECTORY)
file(MAKE_DIRECTORY "${_archive_dir}")

if(EXISTS "${LD_FONT_ARCHIVE}")
    file(SHA256 "${LD_FONT_ARCHIVE}" _archive_sha256)
    if(NOT _archive_sha256 STREQUAL INFILTRATR_MB_CORPO_ARCHIVE_SHA256)
        message(FATAL_ERROR
            "MB Corpo archive hash mismatch: ${_archive_sha256} "
            "(Common requires ${INFILTRATR_MB_CORPO_ARCHIVE_SHA256})")
    endif()
else()
    file(DOWNLOAD
        "${INFILTRATR_MB_CORPO_ARCHIVE_URL}"
        "${LD_FONT_ARCHIVE}.tmp"
        EXPECTED_HASH "SHA256=${INFILTRATR_MB_CORPO_ARCHIVE_SHA256}"
        TLS_VERIFY ON
        STATUS _download_status)
    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
        list(GET _download_status 1 _download_message)
        file(REMOVE "${LD_FONT_ARCHIVE}.tmp")
        message(FATAL_ERROR "Unable to download MB Corpo archive: ${_download_message}")
    endif()
    file(RENAME "${LD_FONT_ARCHIVE}.tmp" "${LD_FONT_ARCHIVE}")
endif()

set(_verify_dir "${LD_FONT_ARCHIVE}.verify")
file(REMOVE_RECURSE "${_verify_dir}")
file(MAKE_DIRECTORY "${_verify_dir}")
file(ARCHIVE_EXTRACT INPUT "${LD_FONT_ARCHIVE}" DESTINATION "${_verify_dir}")

foreach(_role BRAND_REGULAR UI_BOLD UI_REGULAR)
    set(_file_var "INFILTRATR_MB_CORPO_${_role}_FILE")
    set(_sha_var "INFILTRATR_MB_CORPO_${_role}_SHA256")
    set(_path "${_verify_dir}/${${_file_var}}")
    if(NOT EXISTS "${_path}")
        file(REMOVE_RECURSE "${_verify_dir}")
        message(FATAL_ERROR "MB Corpo archive is missing ${${_file_var}}")
    endif()
    file(SHA256 "${_path}" _actual)
    if(NOT _actual STREQUAL "${${_sha_var}}")
        file(REMOVE_RECURSE "${_verify_dir}")
        message(FATAL_ERROR
            "MB Corpo hash mismatch for ${${_file_var}}: ${_actual} "
            "(Common requires ${${_sha_var}})")
    endif()
endforeach()

file(REMOVE_RECURSE "${_verify_dir}")
message(STATUS "Verified canonical MB Corpo assets from Infiltratr Common")
