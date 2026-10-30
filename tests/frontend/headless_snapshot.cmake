# SPDX-License-Identifier: GPL-3.0-or-later
# A snapshot saved with --preset loads in a run without one: the ROM search must exist without a preset.
# cmake -DHEADLESS=<freetvc-headless> -DROM_DIR=<dir> -DOUT=<scratch dir> -P headless_snapshot.cmake
if(NOT EXISTS "${ROM_DIR}/sys22.bin" OR NOT EXISTS "${ROM_DIR}/ext2x.bin")
    message("SKIP: no sys22.bin/ext2x.bin in ${ROM_DIR}")
    return()
endif()
file(MAKE_DIRECTORY "${OUT}")
set(snap "${OUT}/no_preset.tvcs")
execute_process(COMMAND "${HEADLESS}" --preset 64k+-2.2 --rom-dir "${ROM_DIR}" --frames 5 --save-snapshot "${snap}"
                RESULT_VARIABLE saved OUTPUT_QUIET ERROR_VARIABLE save_err)
if(NOT saved EQUAL 0)
    message(FATAL_ERROR "saving the snapshot failed: ${save_err}")
endif()
execute_process(COMMAND "${HEADLESS}" --rom-dir "${ROM_DIR}" --snapshot "${snap}" --frames 1
                RESULT_VARIABLE loaded OUTPUT_QUIET ERROR_VARIABLE load_err)
if(NOT loaded EQUAL 0 OR NOT load_err MATCHES "snapshot .* loaded")
    message(FATAL_ERROR "loading without --preset failed: ${load_err}")
endif()
message("headless snapshot without --preset: passed")
