// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard matrix reads (ghosting, joystick rows), key names and typing order; no ROM images needed.
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/input/key_script.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/machine.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, unsigned long long got, unsigned long long expected) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s: got %llu (0x%llX), expected %llu (0x%llX)\n", what, got, got, expected, expected);
    }
}

#define CHECK_EQ(got, expected) check((got) == (expected), #got, (got), (expected))

std::uint8_t read_row(freetvc::Machine& m, unsigned row) {
    m.io_write(0x03, static_cast<std::uint8_t>(row));
    return m.io_read(0x58);
}

void press(freetvc::Machine& m, freetvc::MatrixKey key, bool down = true) { m.set_key(key.row, key.column, down); }

void test_matrix() {
    freetvc::Machine m;
    const freetvc::MatrixKey a = *freetvc::find_key("A");  // 4:6
    const freetvc::MatrixKey d = *freetvc::find_key("D");  // 4:1
    const freetvc::MatrixKey e = *freetvc::find_key("E");  // 2:1

    press(m, a);
    CHECK_EQ(read_row(m, 4), 0xBFu);
    CHECK_EQ(read_row(m, 2), 0xFFu);
    press(m, e);
    CHECK_EQ(read_row(m, 2), 0xFDu);  // two keys never ghost

    // [OS] p. 87: with A, D and E held the ROM also sees Q (2:6), the fourth corner
    press(m, d);
    CHECK_EQ(read_row(m, 2), 0xBDu);
    CHECK_EQ(read_row(m, 4), 0xBDu);
    CHECK_EQ(read_row(m, 6), 0xFFu);
    press(m, d, false);
    CHECK_EQ(read_row(m, 2), 0xFDu);
    press(m, a, false);
    press(m, e, false);

    // Joystick 2 is row 9; rows 10-15 have no decoder output
    press(m, freetvc::joystick_key(2, freetvc::JoyLine::Up));
    press(m, freetvc::joystick_key(2, freetvc::JoyLine::Fire));
    CHECK_EQ(read_row(m, 9), 0xF5u);
    CHECK_EQ(read_row(m, 8), 0xFFu);
    CHECK_EQ(read_row(m, 12), 0xFFu);
    CHECK_EQ(m.key_down(9, 3) ? 1u : 0u, 1u);
    press(m, freetvc::joystick_key(1, freetvc::JoyLine::Left));
    CHECK_EQ(read_row(m, 8), 0xBFu);
}

void test_names() {
    const auto key = [](const char* name) {
        const auto k = freetvc::find_key(name);
        return k ? k->row * 10u + k->column : 999u;
    };
    CHECK_EQ(key("space"), 75u);
    CHECK_EQ(key("Return"), 54u);
    CHECK_EQ(key("Á"), 53u);
    CHECK_EQ(key("á"), 53u);
    CHECK_EQ(key("ű"), 55u);
    CHECK_EQ(key("\\"), 43u);
    CHECK_EQ(key("left"), 86u);
    CHECK_EQ(key("!"), 999u);  // characters that need SHIFT are not key names

    const auto text = freetvc::decode_utf8("aŐ€");
    CHECK_EQ(text ? text->size() : 0u, 3u);
    CHECK_EQ(text ? static_cast<unsigned>((*text)[1]) : 0u, 0x150u);
    CHECK_EQ(freetvc::decode_utf8("\xC3") ? 1u : 0u, 0u);
    CHECK_EQ(freetvc::is_letter(U'é') ? 1u : 0u, 1u);
    CHECK_EQ(freetvc::is_letter(U'7') ? 1u : 0u, 0u);
}

void test_typing() {
    using freetvc::KeyEvent;
    const freetvc::TypedText typed = freetvc::type_text(U"aA\nb", 100);
    // a: 100-102; A: SHIFT 104, key 105-107, SHIFT up 108; RETURN 110-112; b after 25 frames: 137-139
    const KeyEvent expected[] = {
        {100, {4, 6}, true}, {102, {4, 6}, false}, {104, {6, 3}, true},  {105, {4, 6}, true},
        {107, {4, 6}, false}, {108, {6, 3}, false}, {110, {5, 4}, true}, {112, {5, 4}, false},
        {137, {6, 0}, true}, {139, {6, 0}, false},
    };
    CHECK_EQ(typed.events.size(), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected) && i < typed.events.size(); ++i) {
        const KeyEvent& got = typed.events[i];
        const bool same = got.frame == expected[i].frame && got.key == expected[i].key && got.down == expected[i].down;
        check(same, "typed event", got.frame * 1000 + got.key.row * 10 + got.key.column,
              expected[i].frame * 1000 + expected[i].key.row * 10 + expected[i].key.column);
    }
    CHECK_EQ(typed.end_frame, 141u);
    CHECK_EQ(typed.unsupported, std::u32string::npos);

    const freetvc::TypedText bad = freetvc::type_text(U"ab€", 0);
    CHECK_EQ(bad.unsupported, 2u);
    CHECK_EQ(bad.events.size(), 0u);

    // Every character entry types back through its own key
    for (const freetvc::CharKey& entry : freetvc::char_keys()) {
        const char32_t one[] = {entry.ch, 0};
        const freetvc::TypedText t = freetvc::type_text(one, 0);
        CHECK_EQ(t.events.size(), entry.shift ? 4u : 2u);
    }
}

}  // namespace

int main() {
    test_matrix();
    test_names();
    test_typing();
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
