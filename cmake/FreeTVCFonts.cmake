# SPDX-License-Identifier: GPL-3.0-or-later
# Atkinson Hyperlegible Next + Mono (SIL OFL 1.1): pinned files fetched at configure time, shipped next to the GUI.
# FREETVC_FONT_DIR may point to a local copy; the SHA-256 is checked either way.

set(FREETVC_FONT_DIR "" CACHE PATH "Local directory with the font files (empty = download them)")
set(_next_base "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible-next/7925f50f649b3813257faf2f4c0b381011f434f1")
set(_mono_base "https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible-next-mono/154d50362016cc3e873eb21d242cd0772384c8f9")

# local name | upstream URL | sha256
set(FREETVC_FONT_FILES
    "AtkinsonHyperlegibleNext-Regular.ttf|${_next_base}/fonts/ttf/AtkinsonHyperlegibleNext-Regular.ttf|88ed5c31a71584c7772963b02d04bef1eb7e3d2e9c8b9cb204339b1f82cf432c"
    "AtkinsonHyperlegibleNext-Medium.ttf|${_next_base}/fonts/ttf/AtkinsonHyperlegibleNext-Medium.ttf|dd50b08b3c560846097d23baaaf6a97ffa20dd077115d23c59df68083b9ea05e"
    "AtkinsonHyperlegibleMono-Regular.ttf|${_mono_base}/fonts/ttf/AtkinsonHyperlegibleMono-Regular.ttf|2c9a7b9fabebd5c746a808d8a1ef2f52e618850caad6ec4fd9f5c48f56e1c1af"
    "OFL-Next.txt|${_next_base}/OFL.txt|aca6a428580965d2297d1b718042dd427c2a9443ece3b0d02d758e161e0c4030"
    "OFL-Mono.txt|${_mono_base}/OFL.txt|1ebb31cf7393164f20d10c1d48406cddb5314feff8465531cf1e4ba37e9dd740")

set(FREETVC_FONTS_STAGE "${CMAKE_BINARY_DIR}/fonts")
file(MAKE_DIRECTORY "${FREETVC_FONTS_STAGE}")
foreach(entry IN LISTS FREETVC_FONT_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 url)
    list(GET parts 2 sha)
    set(dest "${FREETVC_FONTS_STAGE}/${name}")
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" have)
        if(have STREQUAL sha)
            continue()
        endif()
    endif()
    if(FREETVC_FONT_DIR)
        file(SHA256 "${FREETVC_FONT_DIR}/${name}" have)
        if(NOT have STREQUAL sha)
            message(FATAL_ERROR "${FREETVC_FONT_DIR}/${name}: SHA-256 ${have}, expected ${sha}")
        endif()
        file(COPY_FILE "${FREETVC_FONT_DIR}/${name}" "${dest}")
    else()
        file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "font download failed: ${url}: ${status}")
        endif()
    endif()
endforeach()
