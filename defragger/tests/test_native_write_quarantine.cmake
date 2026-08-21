# SPDX-License-Identifier: GPL-3.0-or-later
# Execute every production filesystem mutation binary without the audit
# override. Each worker must reject the request before it can inspect a target.

set(LD_WORKER_VARIABLES
    LD_FAT_WORKER
    LD_EXT_WORKER
    LD_NTFS_WORKER
    LD_EXFAT_WORKER
    LD_XFS_WORKER
    LD_AFFS_WORKER
    LD_HFSPLUS_WORKER)

unset(ENV{LINUX_DEFRAGGER_ENABLE_UNAUDITED_WRITES})

foreach(LD_WORKER_VARIABLE IN LISTS LD_WORKER_VARIABLES)
    if(NOT DEFINED ${LD_WORKER_VARIABLE} OR
       NOT EXISTS "${${LD_WORKER_VARIABLE}}")
        message(FATAL_ERROR
            "Missing production worker for quarantine test: ${LD_WORKER_VARIABLE}")
    endif()

    execute_process(
        COMMAND "${${LD_WORKER_VARIABLE}}"
            defrag /linux-defragger-quarantine-must-not-open
        RESULT_VARIABLE LD_RESULT
        OUTPUT_VARIABLE LD_STDOUT
        ERROR_VARIABLE LD_STDERR)

    if(LD_RESULT EQUAL 0)
        message(FATAL_ERROR
            "${LD_WORKER_VARIABLE} accepted mutation without the audit override")
    endif()

    string(TOLOWER "${LD_STDOUT}\n${LD_STDERR}" LD_OUTPUT)
    if(NOT LD_OUTPUT MATCHES "quarantined")
        message(FATAL_ERROR
            "${LD_WORKER_VARIABLE} did not fail at the quarantine boundary: ${LD_OUTPUT}")
    endif()
endforeach()

message(STATUS "All production mutation workers failed closed at the audit quarantine")
