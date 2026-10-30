# SPDX-License-Identifier: GPL-3.0-or-later
# Plain test programs registered with CTest; exit code 77 means "test data not present" (skipped).
# Test data is never committed: it goes into testdata/ (docs/developer-guide.md "Test data").

set(FREETVC_TEST_DATA "${PROJECT_SOURCE_DIR}/testdata" CACHE PATH "Root of the local test data")
if(FREETVC_FETCH_ROMS AND NOT EXISTS "${FREETVC_TEST_DATA}/roms")
    set(freetvc_test_roms "${CMAKE_BINARY_DIR}/roms")
else()
    set(freetvc_test_roms "${FREETVC_TEST_DATA}/roms")
endif()
set(FREETVC_SST_DIR "${FREETVC_TEST_DATA}/sst-z80/v1"
    CACHE PATH "SingleStepTests/z80 v1 directory (JSON files)")
set(FREETVC_ZEX_DIR "${FREETVC_TEST_DATA}/zex"
    CACHE PATH "Directory with the CP/M zexdoc.com and zexall.com")
set(FREETVC_ROM_DIR "${freetvc_test_roms}"
    CACHE PATH "The ROM autofetch's output: sys12/sys21/sys22.bin, ext1x/ext2x.bin and the card ROMs")

function(freetvc_test_program name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE freetvc_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    freetvc_warnings(${name})
endfunction()

freetvc_test_program(core_test core/core_test.cpp)
add_test(NAME core COMMAND core_test)
set_tests_properties(core PROPERTIES LABELS "core")

freetvc_test_program(bus_test core/bus_test.cpp)
add_test(NAME core.bus COMMAND bus_test)
set_tests_properties(core.bus PROPERTIES LABELS "core")

freetvc_test_program(video_test core/video_test.cpp)
add_test(NAME core.video COMMAND video_test)
set_tests_properties(core.video PROPERTIES LABELS "core")

freetvc_test_program(input_test core/input_test.cpp)
add_test(NAME core.input COMMAND input_test)
set_tests_properties(core.input PROPERTIES LABELS "core")

freetvc_test_program(cas_test core/cas_test.cpp)
add_test(NAME core.cas COMMAND cas_test)
set_tests_properties(core.cas PROPERTIES LABELS "core")

freetvc_test_program(tape_test core/tape_test.cpp)
add_test(NAME core.tape COMMAND tape_test)

freetvc_test_program(gamecard_test core/gamecard_test.cpp)
add_test(NAME core.gamecard COMMAND gamecard_test)
set_tests_properties(core.gamecard PROPERTIES LABELS "core")

freetvc_test_program(multichip_test core/multichip_test.cpp)
add_test(NAME core.multichip COMMAND multichip_test)
set_tests_properties(core.multichip PROPERTIES LABELS "core")

freetvc_test_program(sid_card_test core/sid_card_test.cpp)
add_test(NAME core.sid_card COMMAND sid_card_test)
set_tests_properties(core.sid_card PROPERTIES LABELS "core")

freetvc_test_program(multicart_test core/multicart_test.cpp)
target_link_libraries(multicart_test PRIVATE freetvc_frontend_common)
set(FREETVC_MULTICART_DIR "${FREETVC_TEST_DATA}/TVC-Multicart-v2"
    CACHE PATH "dikdom's TVC-Multicart-v2 repository at commit 631b251")
add_test(NAME core.multicart COMMAND multicart_test ${FREETVC_MULTICART_DIR}/software/tvcpla/tvcpla.bin)
set_tests_properties(core.multicart PROPERTIES LABELS "core")
freetvc_test_program(sddual_test core/sddual_test.cpp)
target_link_libraries(sddual_test PRIVATE freetvc_frontend_common)
add_test(NAME core.sddual COMMAND sddual_test)
set_tests_properties(core.sddual PROPERTIES LABELS "core")

freetvc_test_program(serial_test core/serial_test.cpp)
add_test(NAME core.serial COMMAND serial_test)
set_tests_properties(core.serial PROPERTIES LABELS "core")
freetvc_test_program(nanosd_test core/nanosd_test.cpp)
add_test(NAME core.nanosd COMMAND nanosd_test)
set_tests_properties(core.nanosd PROPERTIES LABELS "core")

freetvc_test_program(floppy_test core/floppy_test.cpp)
add_test(NAME core.floppy COMMAND floppy_test)
set_tests_properties(core.floppy PROPERTIES LABELS "core")

freetvc_test_program(card256_test core/card256_test.cpp)
add_test(NAME core.card256 COMMAND card256_test)
set_tests_properties(core.card256 PROPERTIES LABELS "core")
freetvc_test_program(card256_storage_test core/card256_storage_test.cpp)
add_test(NAME core.card256_storage COMMAND card256_storage_test)
set_tests_properties(core.card256_storage PROPERTIES LABELS "core")
freetvc_test_program(card256_vsid_test core/card256_vsid_test.cpp)
add_test(NAME core.card256_vsid COMMAND card256_vsid_test)
set_tests_properties(core.card256_vsid PROPERTIES LABELS "core")
freetvc_test_program(card256_overlay_test core/card256_overlay_test.cpp)
add_test(NAME core.card256_overlay COMMAND card256_overlay_test)
set_tests_properties(core.card256_overlay PROPERTIES LABELS "core")
set_tests_properties(core.tape PROPERTIES LABELS "core")

freetvc_test_program(sound_test core/sound_test.cpp)
add_test(NAME core.sound COMMAND sound_test)
set_tests_properties(core.sound PROPERTIES LABELS "core")

freetvc_test_program(debug_test debug/debug_test.cpp)
target_link_libraries(debug_test PRIVATE freetvc_debug)
add_test(NAME debug.core COMMAND debug_test)
set_tests_properties(debug.core PROPERTIES LABELS "core")

freetvc_test_program(expression_test debug/expression_test.cpp)
target_link_libraries(expression_test PRIVATE freetvc_debug)
add_test(NAME debug.expression COMMAND expression_test)
freetvc_test_program(source_map_test debug/source_map_test.cpp)
target_link_libraries(source_map_test PRIVATE freetvc_debug)
add_test(NAME debug.source_map COMMAND source_map_test)
set_tests_properties(debug.expression PROPERTIES LABELS "core")
set_tests_properties(debug.source_map PROPERTIES LABELS "core")

find_program(FREETVC_SJASMPLUS sjasmplus HINTS "${PROJECT_SOURCE_DIR}/.tools/sjasmplus-1.24.0/bin"
    DOC "sjasmplus for the disassembler round trip")
if(NOT FREETVC_SJASMPLUS)
    set(FREETVC_SJASMPLUS "sjasmplus-not-found")
endif()
freetvc_test_program(disasm_test debug/disasm_test.cpp)
target_link_libraries(disasm_test PRIVATE freetvc_debug)
add_test(NAME debug.disasm COMMAND disasm_test)
set_tests_properties(debug.disasm PROPERTIES LABELS "core")
add_test(NAME debug.disasm_sjasmplus COMMAND disasm_test ${FREETVC_SJASMPLUS} ${CMAKE_CURRENT_BINARY_DIR}/disasm_roundtrip)
set_tests_properties(debug.disasm_sjasmplus PROPERTIES LABELS "core" SKIP_RETURN_CODE 77)

freetvc_test_program(rom_search_test frontend/rom_search_test.cpp)
target_link_libraries(rom_search_test PRIVATE freetvc_frontend_common)
add_test(NAME frontend.rom_search COMMAND rom_search_test ${FREETVC_ROM_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(frontend.rom_search PROPERTIES LABELS "core" SKIP_RETURN_CODE 77)

freetvc_test_program(card256_host_test frontend/card256_host_test.cpp)
target_link_libraries(card256_host_test PRIVATE freetvc_frontend_common)
add_test(NAME frontend.card256_host COMMAND card256_host_test)
set_tests_properties(frontend.card256_host PROPERTIES LABELS "core")
freetvc_test_program(rom_fetch_test frontend/rom_fetch_test.cpp)
target_link_libraries(rom_fetch_test PRIVATE freetvc_frontend_common)
add_test(NAME frontend.rom_fetch COMMAND rom_fetch_test ${FREETVC_ROM_DIR})
set_tests_properties(frontend.rom_fetch PROPERTIES LABELS "core")

freetvc_test_program(settings_test frontend/settings_test.cpp)
target_link_libraries(settings_test PRIVATE freetvc_frontend_common)
add_test(NAME frontend.settings COMMAND settings_test ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(frontend.settings PROPERTIES LABELS "core")

freetvc_test_program(monitor_test remote/monitor_test.cpp)
target_link_libraries(monitor_test PRIVATE freetvc_remote)
add_test(NAME remote.monitor COMMAND monitor_test ${CMAKE_CURRENT_BINARY_DIR}/monitor_scratch)
set_tests_properties(remote.monitor PROPERTIES LABELS "core" TIMEOUT 120)
freetvc_test_program(dzrp_test remote/dzrp_test.cpp)
target_link_libraries(dzrp_test PRIVATE freetvc_remote)
add_test(NAME remote.dzrp COMMAND dzrp_test)
set_tests_properties(remote.dzrp PROPERTIES LABELS "core" TIMEOUT 120)
freetvc_test_program(gdb_test remote/gdb_test.cpp)
target_link_libraries(gdb_test PRIVATE freetvc_remote)
add_test(NAME remote.gdb COMMAND gdb_test)
set_tests_properties(remote.gdb PROPERTIES LABELS "core" TIMEOUT 120)
# PNG screenshots decoded by an independent inflater (Python's zlib), pixel by pixel
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
    freetvc_test_program(png_test remote/png_test.cpp)
    target_link_libraries(png_test PRIVATE freetvc_remote)
    add_test(NAME remote.png COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/remote/png_check.py $<TARGET_FILE:png_test>
                                     ${CMAKE_CURRENT_BINARY_DIR}/png)
    set_tests_properties(remote.png PROPERTIES LABELS "core")
endif()

freetvc_test_program(mcp_test remote/mcp_test.cpp)
target_link_libraries(mcp_test PRIVATE freetvc_remote freetvc_frontend_common freetvc_json)
add_test(NAME remote.mcp COMMAND mcp_test ${FREETVC_ROM_DIR})
set_tests_properties(remote.mcp PROPERTIES LABELS "core" TIMEOUT 120)

# The command line path: a --moncommands script on the 2.2 ROM (registered only when the ROM is there)
if(EXISTS ${FREETVC_ROM_DIR}/sys22.bin)
    add_test(NAME remote.headless_script COMMAND freetvc-headless --preset 64k+-2.2 --rom-dir ${FREETVC_ROM_DIR}
             --moncommands ${CMAKE_CURRENT_SOURCE_DIR}/remote/boot_call.mon)
    set_tests_properties(remote.headless_script PROPERTIES LABELS "rom" TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "#1 \\(Stop on exec 0030\\).*RST 30h : DB 71h.*quit")
endif()

freetvc_test_program(boot_test machine/boot_test.cpp)
target_link_libraries(boot_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME boot.${preset} COMMAND boot_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(boot.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

freetvc_test_program(debug_rom_test machine/debug_rom_test.cpp)
target_link_libraries(debug_rom_test PRIVATE freetvc_debug freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME debug.rom.${preset} COMMAND debug_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(debug.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

set(FREETVC_GOLDEN_DIR "${FREETVC_TEST_DATA}/golden"
    CACHE PATH "Frames captured from WinTVC 1.4.1 (<preset>/<scene>.{json,vram,pgm})")
freetvc_test_program(golden_test machine/golden_test.cpp)
# No 64k+-2.1: WinTVC's preset 4 patches EXT 2.x code with the EXT 1.x tape traps and never reaches BASIC.
# Logo: the OS rewrites the "TV COMPUTER" colours (character rows 42-44 = GIF rows 193-204) at any beam line.
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.2)
    add_test(NAME golden.${preset}.logo COMMAND golden_test ${FREETVC_GOLDEN_DIR} ${preset} logo --skip-rows 193-204)
    foreach(scene prompt mode2 mode16)
        add_test(NAME golden.${preset}.${scene} COMMAND golden_test ${FREETVC_GOLDEN_DIR} ${preset} ${scene})
    endforeach()
    set_tests_properties(golden.${preset}.logo golden.${preset}.prompt golden.${preset}.mode2 golden.${preset}.mode16
        PROPERTIES LABELS "golden" SKIP_RETURN_CODE 77)
endforeach()

freetvc_test_program(snapshot_rom_test machine/snapshot_rom_test.cpp)
target_link_libraries(snapshot_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME snapshot.rom.${preset} COMMAND snapshot_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(snapshot.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

freetvc_test_program(tape_rom_test machine/tape_rom_test.cpp)
target_link_libraries(tape_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 64k-1.2 64k+-2.2)
    add_test(NAME media.tape.${preset} COMMAND tape_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(media.tape.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77)
endforeach()

freetvc_test_program(keymap_rom_test machine/keymap_rom_test.cpp)
target_link_libraries(keymap_rom_test PRIVATE freetvc_frontend_common)
add_test(NAME input.keymap_rom COMMAND keymap_rom_test ${FREETVC_ROM_DIR})
set_tests_properties(input.keymap_rom PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77)

freetvc_test_program(typing_test machine/typing_test.cpp)
target_link_libraries(typing_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME input.typing.${preset} COMMAND typing_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(input.typing.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

freetvc_test_program(media_rom_test machine/media_rom_test.cpp)
target_link_libraries(media_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME media.rom.${preset} COMMAND media_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(media.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

set(FREETVC_DISK_ROM "${FREETVC_ROM_DIR}/hbf.rom"
    CACHE FILEPATH "The HBF disk card's EPROM, VT-DOS 1.1")
set(FREETVC_DISK_IMAGE "${FREETVC_TEST_DATA}/Flipper.dsk"
    CACHE FILEPATH "A VT-DOS disk with FLIPPER.CAS")
freetvc_test_program(disk_rom_test machine/disk_rom_test.cpp)
target_link_libraries(disk_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 64k-1.2 64k+-2.2)
    add_test(NAME media.disk.${preset} COMMAND disk_rom_test ${FREETVC_ROM_DIR} ${preset} ${FREETVC_DISK_ROM} ${FREETVC_DISK_IMAGE}
        ${CMAKE_CURRENT_BINARY_DIR}/disk_scratch)
    set_tests_properties(media.disk.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

set(FREETVC_CARD256_ROM "${FREETVC_ROM_DIR}/card256.rom"
    CACHE FILEPATH "TVC256k++ card ROM, firmware 0.24")
freetvc_test_program(card256_rom_test machine/card256_rom_test.cpp)
target_link_libraries(card256_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 64k-1.2 64k+-2.2)
    add_test(NAME card256.rom.${preset} COMMAND card256_rom_test ${FREETVC_ROM_DIR} ${preset} ${FREETVC_CARD256_ROM})
    set_tests_properties(card256.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

set(FREETVC_NANOSD_ROM "${FREETVC_ROM_DIR}/nanosd.rom"
    CACHE FILEPATH "NanoSD IO Card EEPROM, ROM v0.33")
freetvc_test_program(nanosd_rom_test machine/nanosd_rom_test.cpp)
target_link_libraries(nanosd_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 64k-1.2 64k+-2.2)
    add_test(NAME nanosd.rom.${preset} COMMAND nanosd_rom_test ${FREETVC_ROM_DIR} ${preset} ${FREETVC_NANOSD_ROM}
        ${CMAKE_CURRENT_BINARY_DIR}/nanosd_scratch)
    set_tests_properties(nanosd.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

freetvc_test_program(serial_rom_test machine/serial_rom_test.cpp)
target_link_libraries(serial_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 64k-1.2 64k+-2.2)
    add_test(NAME serial.rom.${preset} COMMAND serial_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(serial.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

freetvc_test_program(sound_rom_test machine/sound_rom_test.cpp)
target_link_libraries(sound_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 32k-1.2 32k+hbm-1.2 64k-1.2 64k+-2.1 64k+-2.2)
    add_test(NAME sound.basic.${preset} COMMAND sound_rom_test ${FREETVC_ROM_DIR} ${preset})
    set_tests_properties(sound.basic.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

if(FREETVC_BUILD_GUI)
    freetvc_test_program(host_input_test sdl/host_input_test.cpp ${PROJECT_SOURCE_DIR}/src/frontend/sdl/host_input.cpp)
    target_link_libraries(host_input_test PRIVATE freetvc_frontend_common freetvc_keymap SDL3::SDL3)
    target_include_directories(host_input_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME input.host COMMAND host_input_test ${FREETVC_ROM_DIR})
    set_tests_properties(input.host PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)

    freetvc_test_program(skin_test sdl/skin_test.cpp)
    target_link_libraries(skin_test PRIVATE freetvc_skin)
    add_test(NAME ui.skin COMMAND skin_test ${PROJECT_SOURCE_DIR}/data/skins)
    set_tests_properties(ui.skin PROPERTIES LABELS "core")
    freetvc_test_program(picture_test sdl/picture_test.cpp)
    target_link_libraries(picture_test PRIVATE freetvc_skin)
    add_test(NAME ui.picture COMMAND picture_test ${PROJECT_SOURCE_DIR}/data/pictures)
    set_tests_properties(ui.picture PROPERTIES LABELS "core")
    freetvc_test_program(keymap_test sdl/keymap_test.cpp)
    target_link_libraries(keymap_test PRIVATE freetvc_keymap)
    add_test(NAME ui.keymap COMMAND keymap_test ${PROJECT_SOURCE_DIR}/data/keymaps)
    set_tests_properties(ui.keymap PROPERTIES LABELS "core")

    freetvc_test_program(translation_test sdl/translation_test.cpp)
    target_link_libraries(translation_test PRIVATE freetvc_skin)
    add_test(NAME ui.translation COMMAND translation_test ${PROJECT_SOURCE_DIR}/data/lang)
    set_tests_properties(ui.translation PROPERTIES LABELS "core")
    # Every tr() string translated in every catalog, with the same printf conversions (tools/lang/po_check.py)
    if(Python3_Interpreter_FOUND)
        file(GLOB FREETVC_TEST_LANGUAGES ${PROJECT_SOURCE_DIR}/data/lang/*.po)
        add_test(NAME ui.translation_coverage COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/lang/po_check.py check
                                                      ${PROJECT_SOURCE_DIR}/src ${FREETVC_TEST_LANGUAGES})
        set_tests_properties(ui.translation_coverage PROPERTIES LABELS "core")
    endif()
endif()

freetvc_test_program(sst_test z80/sst_test.cpp)
add_test(NAME z80.sst_subset COMMAND sst_test ${FREETVC_SST_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/z80/sst_subset.txt)
set_tests_properties(z80.sst_subset PROPERTIES LABELS "z80" SKIP_RETURN_CODE 77 TIMEOUT 1800)

freetvc_test_program(zex_test z80/zex_test.cpp)
foreach(zex zexdoc zexall)
    add_test(NAME z80.${zex} COMMAND zex_test ${FREETVC_ZEX_DIR}/${zex}.com)
    set_tests_properties(z80.${zex} PROPERTIES LABELS "z80;slow" SKIP_RETURN_CODE 77 TIMEOUT 3600)
endforeach()

add_test(NAME headless.version COMMAND freetvc-headless --version)
set_tests_properties(headless.version PROPERTIES LABELS "core" PASS_REGULAR_EXPRESSION "^freeTVC ${PROJECT_VERSION}\n$")
add_test(NAME headless.snapshot_no_preset
         COMMAND ${CMAKE_COMMAND} -DHEADLESS=$<TARGET_FILE:freetvc-headless> -DROM_DIR=${FREETVC_ROM_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/headless_snapshot -P ${CMAKE_CURRENT_SOURCE_DIR}/frontend/headless_snapshot.cmake)
set_tests_properties(headless.snapshot_no_preset PROPERTIES LABELS "rom" SKIP_REGULAR_EXPRESSION "SKIP:")
add_test(NAME headless.bad_option COMMAND freetvc-headless --no-such-option)
set_tests_properties(headless.bad_option PROPERTIES LABELS "core" WILL_FAIL TRUE)

freetvc_test_program(sddual_rom_test machine/sddual_rom_test.cpp)
target_link_libraries(sddual_rom_test PRIVATE freetvc_frontend_common)
set(FREETVC_SDDUAL_ROM "${FREETVC_ROM_DIR}/sddual.rom"
    CACHE FILEPATH "SD Dual firmware 2022.11.09 (sddual.rom)")
set(FREETVC_SDDUAL_FILES "${FREETVC_TEST_DATA}/sdinterface"
    CACHE PATH "The SD interface site's unpacked ZIPs: the updater and the VT-DOS system files (optional)")
foreach(preset 64k+-2.2 64k-1.2)
    if(EXISTS "${FREETVC_SDDUAL_FILES}/sd_dual_2022_11_09_gm/sd_dual_2022_11_09_gm/SDFLASH.CAS")
        add_test(NAME sddual.rom.${preset} COMMAND sddual_rom_test ${FREETVC_ROM_DIR} ${preset} ${FREETVC_SDDUAL_ROM}
            ${FREETVC_SDDUAL_FILES}/sd_dual_2022_11_09_gm/sd_dual_2022_11_09_gm ${FREETVC_SDDUAL_FILES}/system_f_v2)
    else()
        add_test(NAME sddual.rom.${preset} COMMAND sddual_rom_test ${FREETVC_ROM_DIR} ${preset} ${FREETVC_SDDUAL_ROM})
    endif()
    set_tests_properties(sddual.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
freetvc_test_program(multicart_rom_test machine/multicart_rom_test.cpp)
target_link_libraries(multicart_rom_test PRIVATE freetvc_frontend_common)
foreach(preset 64k-1.2 64k+-2.2)
    add_test(NAME multicart.rom.${preset} COMMAND multicart_rom_test ${FREETVC_ROM_DIR} ${preset}
        ${FREETVC_MULTICART_DIR}/software/firmware/multicart_v2.1.14en.bin)
    set_tests_properties(multicart.rom.${preset} PROPERTIES LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
