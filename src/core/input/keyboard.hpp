// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC keyboard matrix as data: key positions, names and the characters the ROM makes of them (tvc-hardware.md §7).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace freetvc {

// One switch of the matrix: row 0-9 (port 03h b3-0), column 0-7 (bit of port 58h); rows 10-11 are joysticks 3 and 4.
struct MatrixKey {
    std::uint8_t row = 0;
    std::uint8_t column = 0;
    friend constexpr bool operator==(MatrixKey, MatrixKey) = default;
};

// Rows 10 and 11 hold the GameCard's joysticks 3 and 4: the TVC reads those rows as FFh, the card reads them
inline constexpr unsigned kMatrixRows = 12;
inline constexpr unsigned kKeyboardRows = 10;
inline constexpr unsigned kMatrixKeys = kMatrixRows * 8;
constexpr unsigned key_index(MatrixKey key) { return key.row * 8u + key.column; }

inline constexpr MatrixKey kKeyDel{5, 0};
inline constexpr MatrixKey kKeyReturn{5, 4};
inline constexpr MatrixKey kKeyShift{6, 3};
inline constexpr MatrixKey kKeyLock{6, 5};
inline constexpr MatrixKey kKeyAlt{7, 0};
inline constexpr MatrixKey kKeyEsc{7, 3};
inline constexpr MatrixKey kKeyCtrl{7, 4};
inline constexpr MatrixKey kKeySpace{7, 5};
inline constexpr MatrixKey kKeyIns{8, 0};

// Joystick lines: joystick 1 is row 8 (the built-in stick and cursor keys, plus one external port), joystick 2 row 9.
// Joysticks 3 and 4 (rows 10, 11) use the GameCard's bits: b0 up, b1 down, b2 left, b3 right, b4 fire; Accel has none (b7).
enum class JoyLine : std::uint8_t { Up = 1, Down = 2, Fire = 3, Accel = 4, Right = 5, Left = 6 };
constexpr MatrixKey joystick_key(unsigned joystick, JoyLine line) {
    if (joystick >= 3) {
        constexpr std::uint8_t kGameCardBits[] = {7, 0, 1, 4, 7, 3, 2};
        return {static_cast<std::uint8_t>(joystick == 3 ? 10 : 11), kGameCardBits[static_cast<unsigned>(line) % 7]};
    }
    return {static_cast<std::uint8_t>(joystick == 2 ? 9 : 8), static_cast<std::uint8_t>(line)};
}

// Key names for scripts and the command line: the unshifted legend ("A", "5", "Á", ";") or a word ("SPACE", "UP").
struct NamedKey {
    std::string_view name;
    MatrixKey key;
};
std::span<const NamedKey> named_keys();
std::optional<MatrixKey> find_key(std::string_view name);  // ASCII names ignore case

// A character the SYS ROM produces from one key, with SHIFT or without, while LOCK is off.
struct CharKey {
    char32_t ch;           // Unicode
    std::uint8_t code;     // TVC character code: ASCII, accented letters 80h-88h (capital) and 90h-98h
    MatrixKey key;
    bool shift;
};
std::span<const CharKey> char_keys();
const CharKey* find_char(char32_t ch);

// Letters, including the accented ones: LOCK changes their case, so host front ends pass the user's SHIFT through.
bool is_letter(char32_t ch);

// UTF-8 to code points; nothing for malformed input.
std::optional<std::u32string> decode_utf8(std::string_view text);

}  // namespace freetvc
