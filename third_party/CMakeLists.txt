# SPDX-License-Identifier: GPL-3.0-or-later
# Third-party code; provenance and licences are listed in THIRD_PARTY.md.

# floooh/chips z80.h (zlib), header-only; compiled once as C11 by src/core/z80/z80_chips.c
add_library(freetvc_z80h INTERFACE)
target_include_directories(freetvc_z80h SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/floooh-chips)

# nlohmann/json 3.12.0 (MIT), single header; used by the MCP server
add_library(freetvc_json INTERFACE)
target_include_directories(freetvc_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann)

# FatFs R0.16 (ChaN, 1-clause BSD) and littlefs v2.11.3 (BSD-3-Clause): the TVC256k++ drives
add_library(freetvc_fatfs STATIC fatfs/ff.c fatfs/ffunicode.c)
target_include_directories(freetvc_fatfs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fatfs)
add_library(freetvc_littlefs STATIC littlefs/lfs.c littlefs/lfs_util.c)
target_include_directories(freetvc_littlefs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/littlefs)
target_compile_definitions(freetvc_littlefs PUBLIC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR LFS_NO_ASSERT)

# ymfm (BSD-3-Clause), its OPL family only: the Multichip card's YM3812
add_library(freetvc_ymfm STATIC ymfm/ymfm_opl.cpp ymfm/ymfm_adpcm.cpp ymfm/ymfm_pcm.cpp)
target_include_directories(freetvc_ymfm SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ymfm)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC cannot see engine_timer_expired's tnum < 2 once its assert is compiled out
    target_compile_options(freetvc_ymfm PRIVATE -Wno-stringop-overflow)
endif()

# reSID (GPL-2.0-or-later), Dag Lem's upstream at ef7873f with our State patch: the SID card
add_library(freetvc_resid STATIC resid/sid.cc resid/voice.cc resid/wave.cc resid/envelope.cc resid/filter.cc resid/extfilt.cc
            resid/pot.cc resid/version.cc)
target_include_directories(freetvc_resid SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/resid)
target_compile_definitions(freetvc_resid PRIVATE VERSION="1.0-pre1")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # sid.cc compares a double with an enum constant (FIR_N, RINGSIZE), deprecated since C++20
    target_compile_options(freetvc_resid PRIVATE -Wno-deprecated-enum-float-conversion)
endif()

# 2dfx portable emulator (MIT), Vaczkó Károly's 2dfx v1.1.0 from kks2003/Ep128web web/src/twodfx: the 2dfx card
file(GLOB FREETVC_TWODFX_EGGS ${CMAKE_CURRENT_SOURCE_DIR}/twodfx/firmware_eggs/egg_*.c)
add_library(freetvc_twodfx STATIC twodfx/core/twodfx_emu.c twodfx/core/twodfx_raster.c twodfx/core/twodfx_3d.c
            twodfx/assets/logo_2dfx_asset.c twodfx/firmware_eggs/firmware_page.c ${FREETVC_TWODFX_EGGS})
set_target_properties(freetvc_twodfx PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_include_directories(freetvc_twodfx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/twodfx/include)
target_include_directories(freetvc_twodfx PRIVATE twodfx/core twodfx/assets twodfx/firmware_eggs
                           twodfx/firmware_eggs/portable)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(freetvc_twodfx PRIVATE -w)
endif()
if(NOT WIN32 OR MINGW)
    find_library(FREETVC_LIBM m)
    if(FREETVC_LIBM)
        target_link_libraries(freetvc_twodfx PRIVATE ${FREETVC_LIBM})
    endif()
endif()

if(NOT FREETVC_BUILD_GUI)
    return()
endif()

# SDL3 (zlib): pinned release tarball, or an installed SDL3 for offline/packaged builds
option(FREETVC_USE_SYSTEM_SDL3 "Use an installed SDL3 (find_package) instead of fetching it" OFF)
set(FREETVC_SDL3_URL
    "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz"
    CACHE STRING "SDL3 source tarball; may be a file:// URL, the SHA256 is checked either way")
set(FREETVC_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

if(FREETVC_USE_SYSTEM_SDL3)
    find_package(SDL3 3.2 REQUIRED CONFIG COMPONENTS SDL3)
else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL ${FREETVC_SDL3_URL}
        URL_HASH SHA256=${FREETVC_SDL3_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(SDL3)
endif()

# Dear ImGui (MIT), docking branch v1.92.9b-docking; demo window file not vendored
add_library(freetvc_imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_sdlrenderer3.cpp
    imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(freetvc_imgui SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
target_compile_definitions(freetvc_imgui PUBLIC
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(freetvc_imgui PUBLIC SDL3::SDL3 ${CMAKE_DL_LIBS})
