// SPDX-License-Identifier: GPL-3.0-or-later
// Boots a preset, types a line ('|' = RETURN), and prints every change of the sound state with the PC after the instruction.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: soundtrace <preset> <line> [frames=700]  (ROMs: FREETVC_ROM_DIR or testdata/roms)\n");
        return 2;
    }
    const auto* preset = freetvc::find_preset(argv[1]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset %s\n", argv[1]);
        return 2;
    }
    const char* rom_dir = std::getenv("FREETVC_ROM_DIR");
    std::string error;
    auto config = freetvc::load_preset(*preset, rom_dir ? rom_dir : "testdata/roms", error);
    if (!config) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    freetvc::Machine m(*config);
    std::u32string text;
    for (const char* p = argv[2]; *p; ++p) text.push_back(*p == '|' ? U'\n' : static_cast<char32_t>(*p));
    const auto space = freetvc::find_key("SPACE");
    std::vector<freetvc::KeyEvent> events{{330, *space, true}, {335, *space, false}};  // past the start-up logo
    const auto typed = freetvc::type_text(text, 400, {}).events;
    events.insert(events.end(), typed.begin(), typed.end());
    std::size_t next = 0;
    const unsigned frames = argc > 3 ? std::stoul(argv[3]) : 700;
    unsigned last_pitch = 0xFFFF, last_amp = 99, last_tone = 2, last_irq = 2;
    for (std::uint64_t f = 0; f < frames; ++f) {
        freetvc::apply_key_events(m, events, next, f);
        const std::uint64_t end = m.ticks() + freetvc::kTStatesPerFrame * 2;
        while (m.ticks() < end) {
            const std::uint64_t reload = m.sound().next_reload();
            m.run({.max_ticks = end - m.ticks(), .max_instructions = 1});
            const auto& s = m.sound();
            const bool instart = reload > m.ticks() && s.next_reload() != reload;  // moved with no reload due
            if (s.pitch() != last_pitch || s.amplitude() != last_amp || s.tone_enabled() != last_tone ||
                s.irq_enabled() != last_irq || instart) {
                std::printf("%10.4f s  f%llu pc %04X  P %03X amp %2u tone %d irq %d chain %2u%s\n", m.ticks() / 6.25e6,
                            static_cast<unsigned long long>(m.frame_count()), m.cpu().registers().pc, s.pitch(),
                            s.amplitude(), s.tone_enabled(), s.irq_enabled(), s.chain(), instart ? "  INSTART" : "");
                last_pitch = s.pitch();
                last_amp = s.amplitude();
                last_tone = s.tone_enabled();
                last_irq = s.irq_enabled();
            }
        }
    }
}
