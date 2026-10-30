// SPDX-License-Identifier: GPL-3.0-or-later
// Timed matrix input for scripts (headless, tests, later the monitor and MCP): key presses and typed text by frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/keyboard.hpp"

namespace freetvc {

class Machine;

struct KeyEvent {
    std::uint64_t frame;
    MatrixKey key;
    bool down;
};

// The OS scans the matrix once per frame (cursor IRQ); a key needs a scan held and a scan released to type twice.
// Keys pressed while BASIC handles a RETURN are lost, hence the longer pause after it: storing a 47-character
// program line needed 15 frames. A command that runs longer needs its own later --type.
struct TypingTiming {
    unsigned hold = 2;           // frames a key stays down
    unsigned gap = 2;            // frames between one key's release and the next press
    unsigned after_return = 25;  // frames after RETURN's release before the next key
};

struct TypedText {
    std::vector<KeyEvent> events;
    std::uint64_t end_frame = 0;               // first frame after the last release
    std::size_t unsupported = std::u32string::npos;  // index of the first character no key produces (nothing typed)
};

// Text as key events from start_frame, LOCK assumed off. A shifted character gets SHIFT one frame before its key
// and releases it one frame after, so no scan can see the key with the wrong SHIFT state.
TypedText type_text(std::u32string_view text, std::uint64_t start_frame, TypingTiming timing = {});

// Applies, in order, every event up to `frame` (events sorted by frame; `next` is the cursor into them).
void apply_key_events(Machine& machine, const std::vector<KeyEvent>& events, std::size_t& next, std::uint64_t frame);

}  // namespace freetvc
