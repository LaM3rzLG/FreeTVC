// SPDX-License-Identifier: GPL-3.0-or-later
// SingleStepTests/z80 through the Z80 adapter: final state, length, refresh addresses and per-access bus timing.
// usage: sst_test <sst_v1_dir> <subset_list>; exit 77 (CTest skip) when the data is not present.
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/z80/z80.hpp"
#include "support/mini_json.hpp"

namespace {

using freetvc::Z80;
using freetvc::test::JsonValue;

constexpr int kExitSkip = 77;

// Mismatch classes; the first three are the ones the known-deviation table may excuse.
enum : unsigned {
    kPc = 1u << 0,
    kFlagsXY = 1u << 1,
    kFlags = 1u << 2,
    kOtherReg = 1u << 3,
    kLength = 1u << 4,
    kRam = 1u << 5,
    kBus = 1u << 6,
    kTiming = 1u << 7,
    kRefresh = 1u << 8,
};

struct KnownDeviation {
    std::string_view file;
    unsigned allowed;
};

// Known z80.h deviations (docs/design/z80-core.md); each must still occur, so a fix forces an update here.
// Empty since our z80.h patches (design/z80-core.md) fixed Q, the block-repeat flags and the HALT PC.
constexpr std::array<KnownDeviation, 0> kKnownDeviations{};

unsigned allowed_for(std::string_view file) {
    for (const auto& known : kKnownDeviations) {
        if (known.file == file) {
            return known.allowed;
        }
    }
    return 0;
}

struct Access {
    int tick;
    char kind;  // F = opcode fetch, R/W = memory, I/O = port
    int addr;
    int data;
};

// SST marks M1 at T2, MR/MW at T2, IOR/IOW at TW; z80.h returns M1 at T1, MR/MW/IOW at T2, IOR at TW.
int expected_offset(char kind) { return (kind == 'F' || kind == 'O') ? 1 : 0; }

int json_int_or(const JsonValue& v, int fallback) { return v.kind == JsonValue::Kind::Number ? v.as_int() : fallback; }

std::array<std::uint8_t, 0x10000> g_mem;

struct Result {
    unsigned mismatch = 0;
    std::string detail;
};

void note(Result& res, unsigned what, const std::string& text) {
    res.mismatch |= what;
    res.detail += " " + text;
}

std::string hex(int v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", static_cast<unsigned>(v));
    return buf;
}

Result run_test(const JsonValue& test) {
    const JsonValue& ini = test.at("initial");
    const JsonValue& fin = test.at("final");
    const JsonValue& cycles = test.at("cycles");
    g_mem.fill(0);
    for (const auto& cell : ini.at("ram").array) {
        g_mem[static_cast<std::uint16_t>(cell.array[0].as_int())] = static_cast<std::uint8_t>(cell.array[1].as_int());
    }

    Z80 cpu;
    freetvc::Z80Registers regs = cpu.registers();
    auto get = [&](const JsonValue& state, const char* key) { return state.at(key).as_int(); };
    regs.sp = static_cast<std::uint16_t>(get(ini, "sp"));
    regs.af = static_cast<std::uint16_t>(get(ini, "a") << 8 | get(ini, "f"));
    regs.bc = static_cast<std::uint16_t>(get(ini, "b") << 8 | get(ini, "c"));
    regs.de = static_cast<std::uint16_t>(get(ini, "d") << 8 | get(ini, "e"));
    regs.hl = static_cast<std::uint16_t>(get(ini, "h") << 8 | get(ini, "l"));
    regs.i = static_cast<std::uint8_t>(get(ini, "i"));
    regs.r = static_cast<std::uint8_t>(get(ini, "r"));
    regs.wz = static_cast<std::uint16_t>(get(ini, "wz"));
    regs.ix = static_cast<std::uint16_t>(get(ini, "ix"));
    regs.iy = static_cast<std::uint16_t>(get(ini, "iy"));
    regs.af2 = static_cast<std::uint16_t>(get(ini, "af_"));
    regs.bc2 = static_cast<std::uint16_t>(get(ini, "bc_"));
    regs.de2 = static_cast<std::uint16_t>(get(ini, "de_"));
    regs.hl2 = static_cast<std::uint16_t>(get(ini, "hl_"));
    regs.im = static_cast<std::uint8_t>(get(ini, "im"));
    regs.iff1 = get(ini, "iff1") != 0;
    regs.iff2 = get(ini, "iff2") != 0;
    regs.q = static_cast<std::uint8_t>(get(ini, "q"));
    cpu.set_registers(regs);
    Z80::Pins pins = cpu.prefetch(static_cast<std::uint16_t>(get(ini, "pc")));

    std::vector<int> port_reads;
    if (const JsonValue* ports = test.find("ports")) {
        for (const auto& p : ports->array) {
            if (p.array[2].string == "r") {
                port_reads.push_back(p.array[1].as_int());
            }
        }
    }
    std::size_t next_port = 0;

    std::vector<Access> accesses;
    std::vector<int> refresh(256, -1);
    int ticks = 0;
    for (; ticks < 200; ++ticks) {
        pins = cpu.tick(pins);
        if (ticks > 0 && cpu.instruction_done()) {
            break;
        }
        const int addr = Z80::address(pins);
        if ((pins & (Z80::kMREQ | Z80::kRFSH)) == (Z80::kMREQ | Z80::kRFSH)) {
            refresh[static_cast<std::size_t>(ticks)] = addr;
        } else if (pins & Z80::kMREQ) {
            if (pins & Z80::kRD) {
                pins = Z80::with_data(pins, g_mem[addr]);
                accesses.push_back({ticks, (pins & Z80::kM1) ? 'F' : 'R', addr, g_mem[addr]});
            } else if (pins & Z80::kWR) {
                g_mem[addr] = Z80::data(pins);
                accesses.push_back({ticks, 'W', addr, Z80::data(pins)});
            }
        } else if (pins & Z80::kIORQ) {
            if (pins & Z80::kRD) {
                const int value = next_port < port_reads.size() ? port_reads[next_port++] : 0xFF;
                pins = Z80::with_data(pins, static_cast<std::uint8_t>(value));
                accesses.push_back({ticks, 'I', addr, value});
            } else if (pins & Z80::kWR) {
                accesses.push_back({ticks, 'O', addr, Z80::data(pins)});
            }
        }
    }

    Result res;
    const int ncyc = static_cast<int>(cycles.array.size());
    if (ticks != ncyc) {
        note(res, kLength, "len " + std::to_string(ticks) + "!=" + std::to_string(ncyc));
    }

    const freetvc::Z80Registers got = cpu.registers();
    const int got_pc = Z80::address(pins);
    const int exp_f = get(fin, "f");
    if (got_pc != get(fin, "pc")) {
        note(res, kPc, "pc " + hex(got_pc) + "!=" + hex(get(fin, "pc")));
    }
    const int got_f = got.af & 0xFF;
    if (got_f != exp_f) {
        note(res, ((got_f ^ exp_f) & ~0x28) ? kFlags : kFlagsXY, "f " + hex(got_f) + "!=" + hex(exp_f));
    }
    const struct {
        const char* key;
        int value;
    } others[] = {
        {"sp", got.sp},       {"a", got.af >> 8},  {"b", got.bc >> 8},   {"c", got.bc & 0xFF}, {"d", got.de >> 8},
        {"e", got.de & 0xFF}, {"h", got.hl >> 8},  {"l", got.hl & 0xFF},  {"i", got.i},         {"r", got.r},
        {"wz", got.wz},       {"ix", got.ix},      {"iy", got.iy},        {"af_", got.af2},     {"bc_", got.bc2},
        {"de_", got.de2},     {"hl_", got.hl2},    {"im", got.im},        {"iff1", got.iff1},   {"iff2", got.iff2},
        {"q", got.q},
    };
    for (const auto& o : others) {
        if (o.value != get(fin, o.key)) {
            note(res, kOtherReg, std::string(o.key) + " " + hex(o.value) + "!=" + hex(get(fin, o.key)));
        }
    }
    for (const auto& cell : fin.at("ram").array) {
        if (g_mem[static_cast<std::uint16_t>(cell.array[0].as_int())] != cell.array[1].as_int()) {
            note(res, kRam, "ram[" + hex(cell.array[0].as_int()) + "]");
            break;
        }
    }

    // Pair our accesses with SST's read/write bus states in order; each kind must sit at its fixed offset.
    auto is_access = [&](int j) {
        const std::string& fl = cycles.array[static_cast<std::size_t>(j)].array[2].string;
        return fl.size() >= 4 && (fl[0] == 'r' || fl[1] == 'w');
    };
    int j = 0;
    bool bus_bad = false;
    bool timing_bad = false;
    for (const Access& a : accesses) {
        while (j < ncyc && !is_access(j)) {
            ++j;
        }
        if (j >= ncyc) {
            bus_bad = true;
            break;
        }
        const JsonValue& c = cycles.array[static_cast<std::size_t>(j)];
        const std::string& fl = c.array[2].string;
        const char sst_kind = fl[3] == 'i' ? (fl[0] == 'r' ? 'I' : 'O') : (fl[0] == 'r' ? 'R' : 'W');
        const char our_kind = a.kind == 'F' ? 'R' : a.kind;
        const int sst_data = json_int_or(c.array[1], -1);
        if (sst_kind != our_kind || json_int_or(c.array[0], -1) != a.addr ||
            (fl[1] == 'w' && sst_data >= 0 && sst_data != a.data)) {
            bus_bad = true;
            break;
        }
        if (j - a.tick != expected_offset(a.kind)) {
            timing_bad = true;
        }
        ++j;
    }
    if (!bus_bad) {
        while (j < ncyc && !is_access(j)) {
            ++j;
        }
        bus_bad = j < ncyc;
    }
    if (bus_bad) {
        note(res, kBus, "bus");
    }
    if (timing_bad) {
        note(res, kTiming, "timing");
    }
    for (int i = 0; i < ticks && i < ncyc; ++i) {
        const int expected = json_int_or(cycles.array[static_cast<std::size_t>(i)].array[0], -1);
        if (refresh[static_cast<std::size_t>(i)] >= 0 && refresh[static_cast<std::size_t>(i)] != expected) {
            note(res, kRefresh, "rfsh");
            break;
        }
    }
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: sst_test <sst_v1_dir> <subset_list>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    if (!std::filesystem::is_directory(dir)) {
        std::printf("SKIP: SingleStepTests directory '%s' not found (set FREETVC_SST_DIR)\n", argv[1]);
        return kExitSkip;
    }
    std::ifstream list(argv[2]);
    if (!list) {
        std::fprintf(stderr, "cannot open subset list '%s'\n", argv[2]);
        return 2;
    }

    long files = 0, tests = 0, unexpected = 0, known = 0, bad_files = 0;
    std::string name;
    while (std::getline(list, name)) {
        if (name.empty() || name[0] == '#') {
            continue;
        }
        const std::filesystem::path path = dir / (name + ".json");
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::printf("FAIL %-6s missing file %s\n", name.c_str(), path.string().c_str());
            ++bad_files;
            continue;
        }
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        JsonValue suite;
        try {
            suite = freetvc::test::JsonParser(text).parse();
        } catch (const std::exception& e) {
            std::printf("FAIL %-6s %s\n", name.c_str(), e.what());
            ++bad_files;
            continue;
        }

        const unsigned allowed = allowed_for(name);
        long file_unexpected = 0, file_known = 0;
        int shown = 0;
        for (const JsonValue& test : suite.array) {
            const Result res = run_test(test);
            ++tests;
            if (res.mismatch == 0) {
                continue;
            }
            if (allowed != 0 && (res.mismatch & ~allowed) == 0) {
                ++file_known;
                continue;
            }
            ++file_unexpected;
            if (shown++ < 3) {
                std::printf("    %s:%s\n", test.at("name").string.c_str(), res.detail.c_str());
            }
        }
        ++files;
        unexpected += file_unexpected;
        known += file_known;
        const bool deviation_gone = allowed != 0 && file_known == 0;
        const bool ok = file_unexpected == 0 && !deviation_gone;
        if (!ok) {
            ++bad_files;
        }
        std::printf("%s %-6s tests %4zu  unexpected %4ld  known deviation %4ld%s\n", ok ? "ok  " : "FAIL", name.c_str(),
                    suite.array.size(), file_unexpected, file_known,
                    deviation_gone ? "  (listed deviation no longer occurs: update kKnownDeviations)" : "");
    }
    std::printf("files %ld  tests %ld  unexpected %ld  known deviation %ld  failed files %ld\n", files, tests,
                unexpected, known, bad_files);
    return bad_files == 0 && files > 0 ? 0 : 1;
}
