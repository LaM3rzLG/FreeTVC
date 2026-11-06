# SPDX-License-Identifier: GPL-3.0-or-later

configure_file(core/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp @ONLY)

# The machine: no OS, threads, clocks or I/O (docs/design/architecture.md)
add_library(freetvc_core STATIC
    core/z80/z80_chips.c
    core/z80/z80.cpp
    core/message.cpp
    core/bus/memory_map.cpp
    core/bus/multicart.cpp
    core/bus/jedec_flash.cpp
    core/bus/sd_card.cpp
    core/bus/sddual.cpp
    core/video/mc6845.cpp
    core/video/video.cpp
    core/input/keyboard.cpp
    core/input/key_script.cpp
    core/audio/synth.cpp
    core/audio/sound.cpp
    core/machine/machine.cpp
    core/cards/card.cpp
    core/cards/card256.cpp
    core/cards/card256_functions.cpp
    core/cards/card256_graphics.cpp
    core/cards/card256_overlay.cpp
    core/cards/card256_storage.cpp
    core/cards/vsid.cpp
    core/cards/vsid_tables.cpp
    core/cards/floppy.cpp
    core/cards/gamecard.cpp
    core/cards/hbf.cpp
    core/cards/i8251.cpp
    core/cards/multichip.cpp
    core/cards/nanosd.cpp
    core/cards/saa1099.cpp
    core/cards/serial_card.cpp
    core/cards/twodfx_card.cpp
    core/cards/sid_card.cpp
    core/cards/sn76489.cpp
    core/cards/ym3812.cpp
    core/cards/wd1793.cpp
    core/media/cas.cpp
    core/media/host_cassette.cpp
    core/media/media.cpp
    core/media/tape_deck.cpp
    core/media/tape_signal.cpp
    core/snapshot/snapshot.cpp
    core/snapshot/machine_state.cpp
    core/snapshot/tvs_import.cpp)
target_include_directories(freetvc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(freetvc_core PRIVATE freetvc_z80h)
target_link_libraries(freetvc_core PUBLIC freetvc_fatfs freetvc_littlefs)
target_link_libraries(freetvc_core PRIVATE freetvc_ymfm freetvc_resid freetvc_twodfx)
freetvc_warnings(freetvc_core)

# Debug API shared by every front end; like the core, no OS, threads, clocks or I/O
# Our ROM symbol files (docs/hardware/rom-symbols) are compiled in, so the docs stay the only copy
file(GLOB FREETVC_ROM_SYMBOL_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/docs/hardware/rom-symbols/*.sym)
list(JOIN FREETVC_ROM_SYMBOL_FILES "|" rom_symbol_list)
set(rom_symbols_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/debug/rom_symbol_files.cpp)
add_custom_command(OUTPUT ${rom_symbols_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${rom_symbols_cpp} -DHEADER=debug/rom_symbol_files.hpp -DFUNCTION=rom_symbol_file
            -DFILES=${rom_symbol_list} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${FREETVC_ROM_SYMBOL_FILES} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    VERBATIM)

add_library(freetvc_debug STATIC
    debug/z80_decode.cpp
    debug/debugger.cpp
    debug/profile.cpp
    debug/source_map.cpp
    debug/symbols.cpp
    debug/expression.cpp
    debug/disassembler.cpp
    ${rom_symbols_cpp})
target_link_libraries(freetvc_debug PUBLIC freetvc_core)
freetvc_warnings(freetvc_debug)

# Remote front ends (design/debug-and-remote.md): sockets, threads and files are allowed here
find_package(Threads REQUIRED)
add_library(freetvc_remote STATIC
    remote/net.cpp
    remote/png.cpp
    remote/monitor.cpp
    remote/monitor_hub.cpp
    remote/mcp.cpp
    remote/mcp_tools.cpp
    remote/mcp_host.cpp
    remote/dzrp.cpp
    remote/dzrp_host.cpp
    remote/gdb.cpp
    remote/gdb_host.cpp)
target_link_libraries(freetvc_remote PUBLIC freetvc_debug Threads::Threads PRIVATE freetvc_json)
if(WIN32)
    target_link_libraries(freetvc_remote PUBLIC ws2_32)
endif()
freetvc_warnings(freetvc_remote)

# Front-end helpers that touch files: presets and ROM loading; the ROM autofetch's list is compiled in
set(rom_list_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/frontend/rom_list_text.cpp)
add_custom_command(OUTPUT ${rom_list_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${rom_list_cpp} -DHEADER=frontend/common/rom_list_text.hpp -DFUNCTION=rom_list_text
            -DNAMESPACE=freetvc -DFILES=${PROJECT_SOURCE_DIR}/data/roms.txt -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/data/roms.txt ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    VERBATIM)
add_library(freetvc_frontend_common STATIC frontend/common/presets.cpp frontend/common/settings.cpp frontend/common/wav.cpp frontend/common/video_recorder.cpp
    frontend/common/host_tape.cpp frontend/common/card256_host.cpp frontend/common/rom_fetch.cpp frontend/common/unzip.cpp
    frontend/common/multicart_image.cpp
    frontend/common/sd_image.cpp frontend/common/printer_host.cpp frontend/common/serial_host.cpp ${rom_list_cpp})
target_link_libraries(freetvc_frontend_common PUBLIC freetvc_core PRIVATE freetvc_debug freetvc_remote)
freetvc_warnings(freetvc_frontend_common)

add_executable(freetvc-headless frontend/headless/main.cpp)
target_link_libraries(freetvc-headless PRIVATE freetvc_frontend_common freetvc_remote)
freetvc_warnings(freetvc-headless)

# Opt-in ROM autofetch: roms/ next to the programs, found by the ROM search
if(FREETVC_FETCH_ROMS)
    include(${PROJECT_SOURCE_DIR}/cmake/FreeTVCRoms.cmake)
    add_custom_command(TARGET freetvc-headless POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${FREETVC_ROMS_STAGE} $<TARGET_FILE_DIR:freetvc-headless>/roms
        VERBATIM)
endif()

if(FREETVC_BUILD_GUI)
    # Skin and picture files, translation catalogs and tr() without SDL/ImGui, shared with the tests
    add_library(freetvc_skin STATIC frontend/sdl/skin.cpp frontend/sdl/picture.cpp frontend/sdl/translation.cpp frontend/sdl/text.cpp)
    target_include_directories(freetvc_skin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(freetvc_skin PUBLIC freetvc_core)
    freetvc_warnings(freetvc_skin)

    # Host key layouts: SDL's scancodes, no window
    add_library(freetvc_keymap STATIC frontend/sdl/keymap.cpp)
    target_include_directories(freetvc_keymap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(freetvc_keymap PUBLIC freetvc_core SDL3::SDL3)
    freetvc_warnings(freetvc_keymap)

    add_executable(freetvc WIN32
        frontend/sdl/main.cpp
        frontend/sdl/about_view.cpp
        frontend/sdl/bench_view.cpp
        frontend/sdl/deck.cpp
        frontend/sdl/display.cpp
        frontend/sdl/display_gl.cpp
        frontend/sdl/display_sdl.cpp
        frontend/sdl/host_input.cpp
        frontend/sdl/keyboard_editor.cpp
        frontend/sdl/play_view.cpp
        frontend/sdl/settings_view.cpp
        frontend/sdl/skin_style.cpp)
    target_link_libraries(freetvc PRIVATE freetvc_frontend_common freetvc_remote freetvc_skin freetvc_keymap freetvc_imgui SDL3::SDL3)
    freetvc_warnings(freetvc)

    # The About window's build line, refreshed at every build (cmake/FreeTVCBuildInfo.cmake)
    add_custom_target(freetvc_build_info
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp
            "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}" "-DPLATFORM=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}"
            -P ${PROJECT_SOURCE_DIR}/cmake/FreeTVCBuildInfo.cmake
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp
        VERBATIM)
    add_dependencies(freetvc freetvc_build_info)

    # Fonts and skins live next to the executable (design/ui.md "Skins")
    include(${PROJECT_SOURCE_DIR}/cmake/FreeTVCFonts.cmake)
    add_custom_command(TARGET freetvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${FREETVC_FONTS_STAGE} $<TARGET_FILE_DIR:freetvc>/fonts
        VERBATIM)
    if(FREETVC_FETCH_ROMS)
        add_custom_command(TARGET freetvc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${FREETVC_ROMS_STAGE} $<TARGET_FILE_DIR:freetvc>/roms
            VERBATIM)
    endif()
    # Skins are copied whenever one changes, not only when the program is relinked
    file(GLOB FREETVC_SKIN_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/skins/*.skin)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/skins.stamp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/skins $<TARGET_FILE_DIR:freetvc>/skins
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/skins.stamp
        DEPENDS ${FREETVC_SKIN_FILES}
        VERBATIM)
    add_custom_target(freetvc_skins ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/skins.stamp)
    add_dependencies(freetvc freetvc_skins)
    # Key layouts too (design/ui.md "Keyboard editor")
    file(GLOB FREETVC_KEYMAP_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/keymaps/*.keys)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/keymaps.stamp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/keymaps $<TARGET_FILE_DIR:freetvc>/keymaps
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/keymaps.stamp
        DEPENDS ${FREETVC_KEYMAP_FILES}
        VERBATIM)
    add_custom_target(freetvc_keymaps ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/keymaps.stamp)
    add_dependencies(freetvc freetvc_keymaps)
    # Picture files too (design/ui.md "Picture")
    file(GLOB FREETVC_PICTURE_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/pictures/*.picture)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pictures.stamp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/pictures $<TARGET_FILE_DIR:freetvc>/pictures
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/pictures.stamp
        DEPENDS ${FREETVC_PICTURE_FILES}
        VERBATIM)
    add_custom_target(freetvc_pictures ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pictures.stamp)
    add_dependencies(freetvc freetvc_pictures)
    # The About window's logo (design/ui.md "About window")
    file(GLOB FREETVC_ABOUT_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/about/*)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/about.stamp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/about $<TARGET_FILE_DIR:freetvc>/about
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/about.stamp
        DEPENDS ${FREETVC_ABOUT_FILES}
        VERBATIM)
    add_custom_target(freetvc_about ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/about.stamp)
    add_dependencies(freetvc freetvc_about)
    # Translations too (design/ui.md "Language")
    file(GLOB FREETVC_LANGUAGE_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/lang/*.po)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lang.stamp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/lang $<TARGET_FILE_DIR:freetvc>/lang
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/lang.stamp
        DEPENDS ${FREETVC_LANGUAGE_FILES}
        VERBATIM)
    add_custom_target(freetvc_lang ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lang.stamp)
    add_dependencies(freetvc freetvc_lang)
endif()
