# SPDX-License-Identifier: GPL-3.0-or-later
# Writes build_info.hpp for the About window (commit, dates, compiler, platform); runs at every build with -P and
# replaces the file only when its text changes. Needs -DSOURCE_DIR= -DOUT= -DCOMPILER= -DPLATFORM=.
set(commit "unknown")
set(commit_date "unknown")
set(modified "false")
find_program(FREETVC_GIT git)
if(FREETVC_GIT AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(COMMAND "${FREETVC_GIT}" -C "${SOURCE_DIR}" rev-parse --short=7 HEAD
        OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0)
        set(commit "${out}")
        execute_process(COMMAND "${FREETVC_GIT}" -C "${SOURCE_DIR}" log -1 --format=%cs
            OUTPUT_VARIABLE commit_date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${FREETVC_GIT}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
            OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT changes STREQUAL "")
            set(modified "true")
        endif()
    endif()
endif()
string(TIMESTAMP build_date "%Y-%m-%d" UTC)
string(REGEX REPLACE "^GNU " "GCC " COMPILER "${COMPILER}")
file(WRITE "${OUT}.tmp" "// Generated at build time by cmake/FreeTVCBuildInfo.cmake.
#pragma once

namespace freetvc::build {

inline constexpr const char* kCommit = \"${commit}\";
inline constexpr bool kModified = ${modified};  // tracked files differed from the commit
inline constexpr const char* kCommitDate = \"${commit_date}\";
inline constexpr const char* kBuildDate = \"${build_date}\";
inline constexpr const char* kCompiler = \"${COMPILER}\";
inline constexpr const char* kPlatform = \"${PLATFORM}\";

}  // namespace freetvc::build
")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
