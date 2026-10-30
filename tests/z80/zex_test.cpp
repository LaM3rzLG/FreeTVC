// SPDX-License-Identifier: GPL-3.0-or-later
// CP/M ZEXDOC/ZEXALL through the Z80 adapter on a flat 64 KiB RAM; BDOS functions 2 and 9 trapped at 0005h.
// usage: zex_test <file.com>; passes when all 67 groups print OK. Exit 77 (CTest skip) when the file is missing.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/z80/z80.hpp"

namespace {

using freetvc::Z80;

constexpr int kExitSkip = 77;
constexpr int kGroups = 67;

std::array<std::uint8_t, 0x10000> g_mem{};

std::uint16_t pop(freetvc::Z80Registers& regs) {
    const std::uint16_t value = static_cast<std::uint16_t>(g_mem[regs.sp] | g_mem[static_cast<std::uint16_t>(regs.sp + 1)] << 8);
    regs.sp = static_cast<std::uint16_t>(regs.sp + 2);
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: zex_test <file.com>\n");
        return 2;
    }
    if (!std::filesystem::is_regular_file(argv[1])) {
        std::printf("SKIP: '%s' not found (set FREETVC_ZEX_DIR)\n", argv[1]);
        return kExitSkip;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> program{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (program.empty() || program.size() > 0xFE00 - 0x0100) {
        std::fprintf(stderr, "bad COM file size %zu\n", program.size());
        return 2;
    }
    std::memcpy(&g_mem[0x0100], program.data(), program.size());
    // The BDOS address word at 0006h gives the program its stack top
    g_mem[0x0006] = 0x00;
    g_mem[0x0007] = 0xFE;

    Z80 cpu;
    freetvc::Z80Registers regs = cpu.registers();
    regs.sp = 0xFE00;
    cpu.set_registers(regs);
    Z80::Pins pins = cpu.prefetch(0x0100);

    std::string output;
    std::uint64_t ticks = 0;
    const std::clock_t start = std::clock();
    for (;;) {
        pins = cpu.tick(pins);
        ++ticks;
        if (cpu.instruction_done()) {
            const std::uint16_t pc = Z80::address(pins);
            if (pc == 0x0000) {
                break;
            }
            if (pc == 0x0005) {
                regs = cpu.registers();
                const std::uint8_t function = static_cast<std::uint8_t>(regs.bc);
                if (function == 2) {
                    output.push_back(static_cast<char>(regs.de & 0xFF));
                    std::putchar(regs.de & 0xFF);
                } else if (function == 9) {
                    for (std::uint16_t a = regs.de; g_mem[a] != '$'; ++a) {
                        output.push_back(static_cast<char>(g_mem[a]));
                        std::putchar(g_mem[a]);
                    }
                } else {
                    std::printf("\nFAIL: unhandled BDOS function %u\n", function);
                    return 1;
                }
                std::fflush(stdout);
                const std::uint16_t ret = pop(regs);
                cpu.set_registers(regs);
                pins = cpu.prefetch(ret);
                continue;
            }
        }
        if (pins & Z80::kMREQ) {
            const std::uint16_t addr = Z80::address(pins);
            if (pins & Z80::kRD) {
                pins = Z80::with_data(pins, g_mem[addr]);
            } else if (pins & Z80::kWR) {
                g_mem[addr] = Z80::data(pins);
            }
        } else if ((pins & Z80::kIORQ) && (pins & Z80::kRD)) {
            pins = Z80::with_data(pins, 0xFF);
        }
    }
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;

    int ok = 0;
    for (std::size_t at = output.find("  OK"); at != std::string::npos; at = output.find("  OK", at + 1)) {
        ++ok;
    }
    const bool errors = output.find("ERROR") != std::string::npos;
    const bool complete = output.find("Tests complete") != std::string::npos;
    std::printf("\n[T-states %llu, CPU %.1f s] groups OK %d/%d%s%s\n", static_cast<unsigned long long>(ticks), seconds, ok,
                kGroups, errors ? ", ERROR reported" : "", complete ? "" : ", no 'Tests complete'");
    return ok == kGroups && !errors && complete ? 0 : 1;
}
