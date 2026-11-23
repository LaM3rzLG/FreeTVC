# SPDX-License-Identifier: GPL-3.0-or-later
# Warns when the TVCnet repository's main branch is not the pinned commit; silent without git or a network.
# As a script: cmake -DREPO=<url> -DCOMMIT=<hash> -P cmake/TvcnetCheck.cmake
function(freetvc_tvcnet_check repo commit)
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_TERMINAL_PROMPT=0 ${GIT_EXECUTABLE} ls-remote ${repo} refs/heads/main
        OUTPUT_VARIABLE out RESULT_VARIABLE result ERROR_QUIET TIMEOUT 10)
    string(REGEX MATCH "^[0-9a-f]+" head "${out}")
    if(NOT result EQUAL 0 OR head STREQUAL "")
        return()
    endif()
    if(NOT head STREQUAL commit)
        string(SUBSTRING ${head} 0 7 head_short)
        string(SUBSTRING ${commit} 0 7 pin_short)
        message(WARNING "TVCnet: the firmware repository's main is at ${head_short}, the build uses ${pin_short}. "
                        "tools/tvcnet/update_pin.sh moves the pin (then build and test).")
    else()
        message(STATUS "TVCnet: the pinned firmware is the latest on main")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    freetvc_tvcnet_check(${REPO} ${COMMIT})
endif()
