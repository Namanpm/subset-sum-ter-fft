# Verifies that every file of the frozen root backends (K2, K2_WIDE,
# QUEUE_DENSE, ARWLF + dispatcher; see FROZEN_ROOT_BACKENDS.md) still has the
# SHA-256 recorded in ter2_root/frozen_root_backends.sha256.  Run by CTest:
#   cmake -DROOT=<source dir> -P ter2_root/scripts/check_frozen_root_backends.cmake
#
# The digest is taken over the content with every CR byte removed, which equals
# the SHA-256 of the committed (LF) git blob.  That makes the check independent
# of core.autocrlf and of the checkout platform while still catching any change
# to the code itself.

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "pass -DROOT=<source dir>")
endif()
set(manifest "${ROOT}/ter2_root/frozen_root_backends.sha256")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "missing ${manifest}")
endif()

file(STRINGS "${manifest}" lines)
set(checked 0)
set(failures "")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)  # a CRLF checkout of the manifest leaves a trailing CR
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^([0-9a-f]+)  (.+)$" matched "${line}")
    if(NOT matched)
        message(FATAL_ERROR "malformed manifest line: ${line}")
    endif()
    set(expected "${CMAKE_MATCH_1}")
    set(path "${CMAKE_MATCH_2}")
    math(EXPR checked "${checked} + 1")
    if(NOT EXISTS "${ROOT}/${path}")
        list(APPEND failures "${path}: missing")
        continue()
    endif()
    file(READ "${ROOT}/${path}" content)
    string(REPLACE "\r" "" content "${content}")
    string(SHA256 actual "${content}")
    if(NOT actual STREQUAL expected)
        list(APPEND failures "${path}: changed")
    endif()
endforeach()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "frozen root backends modified (see FROZEN_ROOT_BACKENDS.md):\n  ${text}")
endif()
message(STATUS "frozen root backends intact: ${checked} files")
