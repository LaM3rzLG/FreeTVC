// SPDX-License-Identifier: GPL-3.0-or-later
// Page decoder, I/O map and VRAM clock stretch; expected values from tvc-hardware.md §1-§4 (no ROM images needed).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "core/machine/machine.hpp"
#include "core/snapshot/machine_state.hpp"
#include "core/snapshot/tvs_import.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, unsigned long long got, unsigned long long expected) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s: got %llu (0x%llX), expected %llu (0x%llX)\n", what, got, got, expected, expected);
    }
}

#define CHECK_EQ(got, expected) check((got) == (expected), #got, (got), (expected))

// Marker ROMs: SYS reads 5Ah, EXT E7h, CART CAh; RAM unit n is filled with A0h + n, VRAM bank n with B0h + n.
freetvc::MachineConfig marked(freetvc::Model model, std::size_t ext_size = 0x2000, bool cart = true) {
    freetvc::MachineConfig config;
    config.model = model;
    config.roms.sys.assign(0x4000, 0x5A);
    config.roms.ext.assign(ext_size, 0xE7);
    if (cart) {
        config.roms.cart.assign(0x4000, 0xCA);
    }
    return config;
}

void mark_ram(freetvc::Machine& m) {
    for (unsigned unit = 0; unit < 4; ++unit) {
        for (unsigned i = 0; i < 0x4000; ++i) {
            m.ram()[unit * 0x4000 + i] = static_cast<std::uint8_t>(0xA0 + unit);
        }
    }
}

void mark_vram(freetvc::Machine& m, unsigned banks) {
    for (unsigned bank = 0; bank < banks; ++bank) {
        m.io_write(0x0F, static_cast<std::uint8_t>(bank << 2));  // page-2 bank (64k+)
        for (unsigned i = 0; i < 0x4000; ++i) {
            m.poke(static_cast<std::uint16_t>(0x8000 + i), static_cast<std::uint8_t>(0xB0 + bank));
        }
    }
    m.io_write(0x0F, 0x00);
}

void paging_reset_state() {
    // Reset value 00h: SYS, U1, VID, CART
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    mark_ram(m);
    mark_vram(m, 1);
    CHECK_EQ(m.memory().page_register(), 0u);
    CHECK_EQ(m.peek(0x0000), 0x5Au);
    CHECK_EQ(m.peek(0x4000), 0xA1u);
    CHECK_EQ(m.peek(0x8000), 0xB0u);
    CHECK_EQ(m.peek(0xC000), 0xCAu);
    CHECK_EQ(static_cast<unsigned>(m.memory().is_video(0x8000)), 1u);
    CHECK_EQ(static_cast<unsigned>(m.memory().is_video(0x4000)), 0u);
}

void paging_fields() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    mark_ram(m);
    m.io_write(0x02, 0x08);  // page 0 = CART
    CHECK_EQ(m.peek(0x0000), 0xCAu);
    m.io_write(0x02, 0x10);  // page 0 = U0
    CHECK_EQ(m.peek(0x0000), 0xA0u);
    m.io_write(0x02, 0x20);  // page 2 = U2
    CHECK_EQ(m.peek(0x8000), 0xA2u);
    m.io_write(0x02, 0x40);  // page 3 = SYS
    CHECK_EQ(m.peek(0xC000), 0x5Au);
    m.io_write(0x02, 0x80);  // page 3 = U3
    CHECK_EQ(m.peek(0xC000), 0xA3u);
    m.io_write(0x02, 0xB0);  // all RAM
    for (const unsigned unit : {0u, 1u, 2u, 3u}) {
        CHECK_EQ(m.peek(static_cast<std::uint16_t>(unit * 0x4000)), 0xA0u + unit);
    }
}

void paging_page0_u3_by_model() {
    // Page-0 field 11: U3 on the 64k+ and with HBM, U0 on the 64k, nothing on a bare 32k
    const std::array<std::pair<freetvc::Model, unsigned>, 4> cases{{{freetvc::Model::Tvc32k, 0xFF},
                                                                    {freetvc::Model::Tvc32kHbm, 0xA3},
                                                                    {freetvc::Model::Tvc64k, 0xA0},
                                                                    {freetvc::Model::Tvc64kPlus, 0xA3}}};
    for (const auto& [model, expected] : cases) {
        freetvc::Machine m(marked(model));
        mark_ram(m);
        m.io_write(0x02, 0x18);
        CHECK_EQ(m.peek(0x0000), expected);
    }
}

void paging_32k_has_no_u2_u3() {
    freetvc::Machine m(marked(freetvc::Model::Tvc32k));
    mark_ram(m);
    m.io_write(0x02, 0xB0);
    CHECK_EQ(m.peek(0x8000), 0xFFu);
    CHECK_EQ(m.peek(0xC000), 0xFFu);
    m.poke(0x8000, 0x12);
    CHECK_EQ(m.peek(0x8000), 0xFFu);
    CHECK_EQ(m.peek(0x4000), 0xA1u);
}

void paging_vid_in_page1() {
    // 02h b2 maps VID into page 1 only on the 64k+; other boards keep U1
    for (const auto model : {freetvc::Model::Tvc64k, freetvc::Model::Tvc64kPlus}) {
        freetvc::Machine m(marked(model));
        mark_ram(m);
        mark_vram(m, model == freetvc::Model::Tvc64kPlus ? 4 : 1);
        m.io_write(0x02, 0x04);
        const bool plus = model == freetvc::Model::Tvc64kPlus;
        CHECK_EQ(m.peek(0x4000), plus ? 0xB0u : 0xA1u);
        CHECK_EQ(static_cast<unsigned>(m.memory().is_video(0x4000)), plus ? 1u : 0u);
    }
}

void video_paging_64k_plus() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    mark_vram(m, 4);
    m.io_write(0x02, 0x04);  // VID in pages 1 and 2
    // b5-4 displayed, b3-2 page 2, b1-0 page 1
    m.io_write(0x0F, 0x39);
    CHECK_EQ(m.peek(0x4000), 0xB1u);
    CHECK_EQ(m.peek(0x8000), 0xB2u);
    CHECK_EQ(m.memory().displayed_bank(), 3u);
    // 0Ch-0Eh are mirrors of the same register ([HW] p.125)
    for (const std::uint8_t port : {0x0C, 0x0D, 0x0E}) {
        m.io_write(0x0F, 0x00);
        m.io_write(port, 0x0B);
        CHECK_EQ(m.peek(0x4000), 0xB3u);
        CHECK_EQ(m.peek(0x8000), 0xB2u);
    }
    // Writes above b5 are dropped
    m.io_write(0x0F, 0xFF);
    CHECK_EQ(m.memory().video_paging(), 0x3Fu);
}

void video_paging_ignored_on_64k() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64k));
    mark_vram(m, 1);
    m.io_write(0x0F, 0x3F);
    CHECK_EQ(m.memory().video_paging(), 0u);
    CHECK_EQ(m.peek(0x8000), 0xB0u);
}

void iomem_and_ext() {
    // Page 3 = 11: C000-DFFF = IOMEM of the selected slot (no card: FFh), E000-FFFF = EXT
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    m.io_write(0x02, 0xC0);
    CHECK_EQ(m.peek(0xC000), 0xFFu);
    CHECK_EQ(m.peek(0xDFFF), 0xFFu);
    CHECK_EQ(m.peek(0xE000), 0xE7u);
    CHECK_EQ(m.peek(0xFFFF), 0xE7u);
    m.io_write(0x03, 0xC0);
    CHECK_EQ(m.memory().iomem_slot(), 3u);

    // A 4 KiB EXT (1.x) sits in the upper half: E000-EFFF read FFh
    freetvc::Machine m1(marked(freetvc::Model::Tvc64k, 0x1000));
    m1.io_write(0x02, 0xC0);
    CHECK_EQ(m1.peek(0xEFFF), 0xFFu);
    CHECK_EQ(m1.peek(0xF000), 0xE7u);
}

void rom_writes_ignored() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus, 0x2000, false));
    m.poke(0x0000, 0x00);
    CHECK_EQ(m.peek(0x0000), 0x5Au);
    // No cartridge: page 3 reads open bus
    CHECK_EQ(m.peek(0xC000), 0xFFu);
}

void io_reads() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    // 59h: slots idle (b3-0 = 1), no cursor/sound IRQ (b4 = 1), tape in 0, colour (b6 = 1), printer ACK (b7 = 1)
    CHECK_EQ(m.io_read(0x59), 0xDFu);
    CHECK_EQ(m.io_read(0x5D), 0xDFu);
    CHECK_EQ(m.io_read(0x5A), 0xFFu);
    CHECK_EQ(m.io_read(0x10), 0xFFu);
    CHECK_EQ(m.io_read(0x80), 0xFFu);
    CHECK_EQ(m.io_read(0x02), 0xFFu);

    freetvc::MachineConfig mono = marked(freetvc::Model::Tvc64k);
    mono.colour = false;
    freetvc::Machine bw(mono);
    CHECK_EQ(bw.io_read(0x59), 0x9Fu);
}

void io_crtc_readback() {
    freetvc::Machine m;
    for (const std::uint8_t reg : {12, 14, 15}) {
        m.io_write(0x70, reg);
        m.io_write(0x71, 0x2A);
    }
    m.io_write(0x70, 14);
    CHECK_EQ(m.io_read(0x71), 0x2Au);
    m.io_write(0x7E, 15);  // even 70h-7Eh = address, odd 71h-7Fh = data
    CHECK_EQ(m.io_read(0x7F), 0x2Au);
    m.io_write(0x70, 12);
    CHECK_EQ(m.io_read(0x71), 0x00u);  // MC6845: R12 is write-only
    CHECK_EQ(m.io_read(0x70), 0xFFu);
    // A7 = 1 is not decoded
    m.io_write(0xF0, 14);
    m.io_write(0x70, 14);
    CHECK_EQ(m.io_read(0x71), 0x2Au);
}

void keyboard_matrix() {
    freetvc::Machine m;
    m.set_key(7, 5, true);  // SPACE
    m.io_write(0x03, 0x07);
    CHECK_EQ(m.io_read(0x58), 0xDFu);
    CHECK_EQ(m.io_read(0x5C), 0xDFu);
    m.io_write(0x03, 0x06);
    CHECK_EQ(m.io_read(0x58), 0xFFu);
    m.io_write(0x03, 0x0F);  // rows 10-15 read FFh
    CHECK_EQ(m.io_read(0x58), 0xFFu);
    m.set_key(7, 5, false);
    m.io_write(0x03, 0x07);
    CHECK_EQ(m.io_read(0x58), 0xFFu);
}

// Instruction boundaries in ticks while code runs; the first boundary only ends the start-up fetch.
std::vector<std::uint64_t> boundary_ticks(freetvc::Machine& m, std::uint16_t pc, int count) {
    m.jump(pc);
    std::vector<std::uint64_t> at;
    for (int i = 0; i < count; ++i) {
        m.run({.max_ticks = 1000, .max_instructions = 1});
        at.push_back(m.ticks());
    }
    return at;
}

void stretch_m1_from_vram() {
    // NOPs in VID: the first M1 starts at DEB phase 0 (+3 half-T); every later one at phase 3 (+4)
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    const std::vector<std::uint64_t> at = boundary_ticks(m, 0x8000, 4);
    CHECK_EQ(at[0], 11u + 6u);  // NOP (8 + 3), then T1 of the next fetch (2 + 4)
    CHECK_EQ(at[1] - at[0], 12u);
    CHECK_EQ(at[2] - at[1], 12u);
    CHECK_EQ(at[3] - at[2], 12u);
}

void stretch_read_from_ram_code() {
    // LD A,(8000h) from U0: only the data read is in VID. Its T1 is at phase 0 (+2): 13 T + 1 T
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    m.io_write(0x02, 0x10);  // U0, U1, VID, CART
    for (std::uint16_t a = 0x100; a < 0x130; a += 3) {
        m.ram()[a] = 0x3A;
        m.ram()[a + 1u] = 0x00;
        m.ram()[a + 2u] = 0x80;
    }
    const std::vector<std::uint64_t> at = boundary_ticks(m, 0x0100, 5);
    // The first instruction pins the phase: z80.h raises MR in T2, so taking the phase there (2, +4) would cost 30 ticks.
    // Later instructions cannot tell the two apart: both settle at 28.
    CHECK_EQ(at[0], 28u + 2u);
    for (std::size_t i = 1; i < at.size(); ++i) {
        CHECK_EQ(at[i] - at[i - 1], 28u);
    }
}

void no_stretch_outside_vram() {
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    m.io_write(0x02, 0xB0);
    const std::vector<std::uint64_t> at = boundary_ticks(m, 0x8000, 3);
    CHECK_EQ(at[1] - at[0], 8u);
    CHECK_EQ(at[2] - at[1], 8u);
}

void irq_flipflop_cleared_by_07h() {
    // Default CRTC values make the cursor pulse once per frame; DI keeps the CPU from acknowledging it
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    m.io_write(0x02, 0xB0);
    m.ram()[0] = 0xF3;  // DI, then NOPs
    m.jump(0x0000);
    const std::array<std::uint8_t, 16> crtc{99, 64, 75, 50, 77, 2, 60, 66, 0, 3, 3, 3, 0, 0, 14, 255};
    for (std::uint8_t r = 0; r < crtc.size(); ++r) {
        m.io_write(0x70, r);
        m.io_write(0x71, crtc[r]);
    }
    m.run({.max_ticks = 2 * freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 1u);
    CHECK_EQ(m.io_read(0x59) & 0x10u, 0u);
    m.io_write(0x07, 0x00);
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 0u);
    CHECK_EQ(m.io_read(0x59) & 0x10u, 0x10u);
}

// Cartridge images whose page n is filled with base + n (design/media.md "Cartridges")
freetvc::MachineConfig with_cart(freetvc::CartType type, unsigned pages, std::uint8_t base, unsigned bank = 0) {
    freetvc::MachineConfig config = marked(freetvc::Model::Tvc64kPlus, 0x2000, false);
    for (unsigned page = 0; page < pages; ++page) {
        config.roms.cart.insert(config.roms.cart.end(), 0x4000, static_cast<std::uint8_t>(base + page));
    }
    config.roms.cart_type = type;
    config.roms.cart_bank = bank;
    return config;
}

// Runs code placed in U0 at 0100h with U0, U1, VID, CART paged in; the code ends with HALT.
void run_from_u0(freetvc::Machine& m, std::initializer_list<std::uint8_t> code) {
    m.io_write(0x02, 0x10);
    std::uint16_t a = 0x0100;
    for (const std::uint8_t byte : code) {
        m.ram()[a++] = byte;
    }
    m.ram()[a] = 0x76;
    m.jump(0x0100);
    m.run({.max_ticks = 10000, .max_instructions = 64});
}

void cart_plain() {
    // A short image reads FFh past its end; no cartridge reads FFh and its bank has no bytes
    freetvc::MachineConfig config = marked(freetvc::Model::Tvc64kPlus, 0x2000, false);
    config.roms.cart.assign(0x2000, 0xC8);
    freetvc::Machine m(config);
    CHECK_EQ(m.peek(0xC000), 0xC8u);
    CHECK_EQ(m.peek(0xDFFF), 0xC8u);
    CHECK_EQ(m.peek(0xE000), 0xFFu);
    freetvc::Machine none(marked(freetvc::Model::Tvc64kPlus, 0x2000, false));
    CHECK_EQ(none.peek(0xC000), 0xFFu);
    CHECK_EQ(none.bank_bytes(freetvc::Bank::Cart).size(), 0u);

    // The board's bank switches pick the 16 KiB window; CPU writes change nothing
    freetvc::Machine banked(with_cart(freetvc::CartType::Plain, 2, 0x60, 1));
    CHECK_EQ(banked.peek(0xC000), 0x61u);
    run_from_u0(banked, {0x3E, 0x00, 0x32, 0x00, 0xFC, 0x3A, 0xFF, 0xFF, 0x32, 0x00, 0x02});  // LD (FC00h),0; LD A,(FFFFh)
    CHECK_EQ(banked.ram()[0x0200], 0x61u);
    CHECK_EQ(banked.memory().cart_page(), 1u);
    CHECK_EQ(banked.bank_bytes(freetvc::Bank::Cart)[0], 0x61u);
}

void cart_kilocart() {
    freetvc::Machine m(with_cart(freetvc::CartType::KiloCart, 4, 0x10));
    CHECK_EQ(m.peek(0xC000), 0x10u);
    // Debugger reads never page
    CHECK_EQ(m.peek(0xFFFE), 0x10u);
    CHECK_EQ(m.memory().cart_page(), 0u);
    // LD A,(FFFEh): page 2, and the byte comes from the new page (YATE ❓); LD A,(C000h)
    run_from_u0(m, {0x3A, 0xFE, 0xFF, 0x32, 0x00, 0x02, 0x3A, 0x00, 0xC0, 0x32, 0x01, 0x02});
    CHECK_EQ(m.ram()[0x0200], 0x12u);
    CHECK_EQ(m.ram()[0x0201], 0x12u);
    CHECK_EQ(m.memory().cart_page(), 2u);
    // Writes do nothing; the reset button keeps the page, power-on clears it
    run_from_u0(m, {0x32, 0xFF, 0xFF, 0x3A, 0x00, 0xC0, 0x32, 0x00, 0x02});
    CHECK_EQ(m.ram()[0x0200], 0x12u);
    m.reset();
    CHECK_EQ(m.memory().cart_page(), 2u);
    m.power_on();
    CHECK_EQ(m.memory().cart_page(), 0u);
    // The same register in page 0 (02h page-0 field 01: CART), read at 3FFDh
    m.io_write(0x02, 0x08);
    m.ram()[0x8000] = 0x76;
    std::uint16_t a = 0x9000;
    for (const std::uint8_t byte : {0x3A, 0xFD, 0x3F, 0x76}) {
        m.poke(a++, byte);
    }
    m.jump(0x9000);
    m.run({.max_ticks = 1000, .max_instructions = 8});
    CHECK_EQ(m.memory().cart_page(), 1u);
}

void cart_megacart() {
    // Three pages present: page 5 reads FFh
    freetvc::Machine m(with_cart(freetvc::CartType::MegaCart, 3, 0x40));
    // LD A,2; LD (FC00h),A; LD A,(C000h); LD (0200h),A; LD A,45h; LD (FFFFh),A; LD A,(C000h); LD (0201h),A
    run_from_u0(m, {0x3E, 0x02, 0x32, 0x00, 0xFC, 0x3A, 0x00, 0xC0, 0x32, 0x00, 0x02, 0x3E, 0x45, 0x32, 0xFF, 0xFF,
                    0x3A, 0x00, 0xC0, 0x32, 0x01, 0x02});
    CHECK_EQ(m.ram()[0x0200], 0x42u);
    CHECK_EQ(m.ram()[0x0201], 0xFFu);
    CHECK_EQ(m.memory().cart_page(), 5u);  // 45h & 3Fh
    CHECK_EQ(m.bank_bytes(freetvc::Bank::Cart).size(), 0u);
    // Writes below 3C00h and debugger writes leave the page alone
    run_from_u0(m, {0x3E, 0x01, 0x32, 0xFF, 0xFB});
    CHECK_EQ(m.memory().cart_page(), 5u);
    m.poke(0xFC00, 0x00);
    CHECK_EQ(m.memory().cart_page(), 5u);
}

void cart_in_snapshot() {
    // The image is embedded and the page register travels; another cartridge is refused
    const freetvc::MachineConfig config = with_cart(freetvc::CartType::MegaCart, 3, 0x40);
    freetvc::Machine m(config);
    run_from_u0(m, {0x3E, 0x02, 0x32, 0x00, 0xFC});
    freetvc::Snapshot s;
    m.save_state(s);
    std::string error;
    const auto decoded = freetvc::Snapshot::decode(s.encode(), error);
    const auto needs = decoded ? freetvc::snapshot_needs(*decoded, error) : std::nullopt;
    CHECK_EQ(static_cast<unsigned>(needs.has_value()), 1u);
    if (!needs) {
        return;
    }
    CHECK_EQ(needs->cart.size(), 3u * 0x4000);
    CHECK_EQ(static_cast<unsigned>(needs->cart_type), static_cast<unsigned>(freetvc::CartType::MegaCart));
    freetvc::Machine fresh(freetvc::apply_needs(marked(freetvc::Model::Tvc64kPlus, 0x2000, false), *needs));
    CHECK_EQ(static_cast<unsigned>(fresh.load_state(*decoded, error)), 1u);
    CHECK_EQ(fresh.memory().cart_page(), 2u);
    CHECK_EQ(fresh.peek(0xC000), 0x42u);
    freetvc::Machine other(with_cart(freetvc::CartType::MegaCart, 3, 0x50));
    CHECK_EQ(static_cast<unsigned>(other.load_state(*decoded, error)), 0u);
}

void tvs_import() {
    // A synthetic WinTVC file with the layout of file-formats.md "WinTVC .tvs"
    std::vector<std::uint8_t> f(freetvc::kTvsSize, 0);
    const std::pair<std::size_t, const char*> tags[] = {{0x00000, "TS01"}, {0x00004, "CPU "}, {0x00063, "IOUT"}, {0x0016B, "IOIN"},
                                                        {0x0026F, "BRAM"}, {0x10273, "VRAM"}, {0x18277, "CRTC"}, {0x18413, "IRQ "}};
    for (const auto& [at, tag] : tags) {
        std::copy_n(tag, 4, f.begin() + static_cast<std::ptrdiff_t>(at));
    }
    const std::size_t cpu = 0x08;
    const std::uint16_t words[] = {0xA1D5, 0xB1C1, 0xD1E1, 0x4151, 0x1F2F, 0x3F4F, 0x5F6E, 0x0123, 0xA2D7, 0xB2C2, 0xD2E2, 0x4252};
    for (std::size_t i = 0; i < std::size(words); ++i) {
        f[cpu + 2 * i] = static_cast<std::uint8_t>(words[i]);
        f[cpu + 2 * i + 1] = static_cast<std::uint8_t>(words[i] >> 8);
    }
    f[cpu + 0x18] = 0x5A;  // I
    f[cpu + 0x1A] = 1;     // IFF1
    f[cpu + 0x1B] = 1;     // IFF2
    f[cpu + 0x1C] = 2;     // IM
    f[0x67 + 0x02] = 0xB0;  // all RAM
    f[0x67 + 0x00] = 0x05;  // border
    f[0x18277 + 4 + 0x48 + 1] = 0x40;  // CRTC R1
    f[0x26F + 4 + 0x0123] = 0xED;      // RAM at the PC
    f[0x10273 + 4 + 0x10] = 0xBE;      // VRAM bank 0
    freetvc::Machine m(marked(freetvc::Model::Tvc64kPlus));
    std::string error;
    CHECK_EQ(static_cast<unsigned>(freetvc::import_tvs(m, f, error)), 1u);
    const freetvc::Z80Registers r = m.cpu().registers();
    CHECK_EQ(r.af, 0xA1D5u);
    CHECK_EQ(r.sp, 0x5F6Eu);
    CHECK_EQ(r.hl2, 0x4252u);
    CHECK_EQ(r.i, 0x5Au);
    CHECK_EQ(static_cast<unsigned>(r.iff1 && r.iff2), 1u);
    CHECK_EQ(r.im, 2u);
    CHECK_EQ(m.pc(), 0x0123u);
    CHECK_EQ(m.memory().page_register(), 0xB0u);
    CHECK_EQ(m.peek(0x0123), 0xEDu);
    CHECK_EQ(m.memory().vram()[0x10], 0xBEu);
    CHECK_EQ(m.video().crtc().registers()[1], 0x40u);
    // A wrong size or a missing tag is refused before anything changes
    f[0x0016B] = 'X';
    const std::uint64_t ticks = m.ticks();
    CHECK_EQ(static_cast<unsigned>(freetvc::import_tvs(m, f, error)), 0u);
    CHECK_EQ(m.ticks(), ticks);
}

void packbits_round_trip() {
    freetvc::ChunkWriter w;
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = i < 300 ? 0xAA : i < 302 ? 0x01 : static_cast<std::uint8_t>(i * 37);
    }
    w.packed(data);
    w.packed(std::vector<std::uint8_t>{});
    freetvc::ChunkReader r(w.data);
    const std::vector<std::uint8_t> first = r.packed_vector(4096);
    const std::vector<std::uint8_t> second = r.packed_vector(4096);
    CHECK_EQ(static_cast<unsigned>(first == data), 1u);
    CHECK_EQ(second.size(), 0u);
    CHECK_EQ(static_cast<unsigned>(r.failed() || !r.at_end()), 0u);
    CHECK_EQ(static_cast<unsigned>(w.data.size() < data.size()), 1u);
}

}  // namespace

int main() {
    cart_in_snapshot();
    packbits_round_trip();
    tvs_import();
    cart_plain();
    cart_kilocart();
    cart_megacart();
    paging_reset_state();
    paging_fields();
    paging_page0_u3_by_model();
    paging_32k_has_no_u2_u3();
    paging_vid_in_page1();
    video_paging_64k_plus();
    video_paging_ignored_on_64k();
    iomem_and_ext();
    rom_writes_ignored();
    io_reads();
    io_crtc_readback();
    keyboard_matrix();
    stretch_m1_from_vram();
    stretch_read_from_ram_code();
    no_stretch_outside_vram();
    irq_flipflop_cleared_by_07h();
    if (g_failures == 0) {
        std::printf("bus_test: all checks passed\n");
        return 0;
    }
    std::printf("bus_test: %d check(s) failed\n", g_failures);
    return 1;
}
