// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/input/key_script.hpp"

#include "core/machine/machine.hpp"

namespace freetvc {

TypedText type_text(std::u32string_view text, std::uint64_t start_frame, TypingTiming timing) {
    TypedText typed;
    std::uint64_t frame = start_frame;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char32_t ch = text[i] == U'\n' ? U'\r' : text[i];
        const CharKey* entry = find_char(ch);
        if (!entry) {
            typed.events.clear();
            typed.unsupported = i;
            typed.end_frame = start_frame;
            return typed;
        }
        if (entry->shift) {
            typed.events.push_back({frame++, kKeyShift, true});
        }
        typed.events.push_back({frame, entry->key, true});
        frame += timing.hold;
        typed.events.push_back({frame, entry->key, false});
        if (entry->shift) {
            typed.events.push_back({++frame, kKeyShift, false});
        }
        frame += entry->key == kKeyReturn ? timing.after_return : timing.gap;
    }
    typed.end_frame = frame;
    return typed;
}

void apply_key_events(Machine& machine, const std::vector<KeyEvent>& events, std::size_t& next, std::uint64_t frame) {
    while (next < events.size() && events[next].frame <= frame) {
        const KeyEvent& event = events[next++];
        machine.set_key(event.key.row, event.key.column, event.down);
    }
}

}  // namespace freetvc
