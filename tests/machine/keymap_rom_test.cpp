// SPDX-License-Identifier: GPL-3.0-or-later
// Checks core/input/keyboard.cpp against the key tables in each SYS ROM (tvc-hardware.md §7): 80 bytes unshifted,
// then 80 shifted, entry (7 - column) * 10 + row. Both directions: every map entry and every character key of rows 0-7.
// usage: keymap_rom_test <rom dir>; exit 77 (CTest skip) when the ROM files are missing.
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/input/keyboard.hpp"
#include "frontend/common/presets.hpp"

namespace {

constexpr int kExitSkip = 77;

struct RomTable {
    const char* file;
    std::size_t offset;  // file offset of the unshifted table (2.x = address CD28h)
};

constexpr RomTable kRoms[] = {{"sys12.bin", 0x17BF}, {"sys21.bin", 0x0D28}, {"sys22.bin", 0x0D28}};

bool modifier_or_del(freetvc::MatrixKey key) {
    return key == freetvc::kKeyShift || key == freetvc::kKeyLock || key == freetvc::kKeyAlt ||
           key == freetvc::kKeyCtrl || key == freetvc::kKeyDel;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: keymap_rom_test <rom dir>\n");
        return 2;
    }
    unsigned failures = 0;
    unsigned checked = 0;
    for (const RomTable& rom : kRoms) {
        std::string error;
        const auto image = freetvc::read_file(std::string(argv[1]) + "/" + rom.file, 0x4000, error);
        if (!image) {
            std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
            return kExitSkip;
        }
        const auto table = [&](freetvc::MatrixKey key, bool shift) {
            return (*image)[rom.offset + (shift ? 80u : 0u) + (7u - key.column) * 10u + key.row];
        };

        for (const freetvc::CharKey& entry : freetvc::char_keys()) {
            ++checked;
            if (table(entry.key, entry.shift) != entry.code) {
                ++failures;
                std::printf("FAIL %s: U+%04X at %u:%u shift %d: ROM %02Xh, map %02Xh\n", rom.file,
                            static_cast<unsigned>(entry.ch), entry.key.row, entry.key.column, entry.shift ? 1 : 0,
                            table(entry.key, entry.shift), entry.code);
            }
        }
        for (std::uint8_t row = 0; row < 8; ++row) {
            for (std::uint8_t column = 0; column < 8; ++column) {
                const freetvc::MatrixKey key{row, column};
                for (const bool shift : {false, true}) {
                    if (modifier_or_del(key)) {
                        continue;
                    }
                    bool found = false;
                    for (const freetvc::CharKey& entry : freetvc::char_keys()) {
                        found = found || (entry.key == key && entry.shift == shift && entry.code == table(key, shift));
                    }
                    // SPACE, RETURN and ESC give the same code both ways; the map types them unshifted only
                    found = found || (shift && table(key, true) == table(key, false));
                    if (!found) {
                        ++failures;
                        std::printf("FAIL %s: ROM %02Xh at %u:%u shift %d has no map entry\n", rom.file, table(key, shift),
                                    row, column, shift ? 1 : 0);
                    }
                }
            }
        }
    }
    std::printf("%u map entries checked against %zu ROMs, %u failures\n%s\n", checked, std::size(kRoms), failures,
                failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
