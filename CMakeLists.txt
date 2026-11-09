# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

project(freeTVC
    VERSION 1.3.3
    DESCRIPTION "Videoton TV Computer emulator"
    LANGUAGES C CXX)

option(FREETVC_BUILD_GUI "Build the SDL3 + Dear ImGui front end" ON)
option(FREETVC_BUILD_TESTS "Build the test programs" ON)
option(FREETVC_WERROR "Treat warnings in freeTVC's own code as errors" OFF)
option(FREETVC_FETCH_ROMS "Download the TVC ROMs from tvc.homeserver.hu into roms/ next to the programs" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MINGW)
    # Self-contained .exe files: no libgcc/libstdc++/winpthread DLLs next to them
    add_link_options(-static)
endif()

include(cmake/FreeTVCWarnings.cmake)

add_subdirectory(third_party)
add_subdirectory(src)

if(FREETVC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
