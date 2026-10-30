// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/input/keyboard.hpp"

namespace freetvc {

namespace {

// Matrix after [HW] fig. 10, one matrix row per line; the characters are checked against the SYS ROM tables by keymap_rom_test.
constexpr NamedKey kNamedKeys[] = {
    {"5", {0, 0}}, {"3", {0, 1}}, {"2", {0, 2}}, {"0", {0, 3}}, {"6", {0, 4}}, {"Í", {0, 5}}, {"1", {0, 6}}, {"4", {0, 7}},
    {"^", {1, 0}}, {"8", {1, 1}}, {"9", {1, 2}}, {"Ü", {1, 3}}, {"*", {1, 4}}, {"Ó", {1, 5}}, {"Ö", {1, 6}}, {"7", {1, 7}},
    {"T", {2, 0}}, {"E", {2, 1}}, {"W", {2, 2}}, {";", {2, 3}}, {"Z", {2, 4}}, {"@", {2, 5}}, {"Q", {2, 6}}, {"R", {2, 7}},
    {"]", {3, 0}}, {"I", {3, 1}}, {"O", {3, 2}}, {"Ő", {3, 3}}, {"[", {3, 4}}, {"Ú", {3, 5}}, {"P", {3, 6}}, {"U", {3, 7}},
    {"G", {4, 0}}, {"D", {4, 1}}, {"S", {4, 2}}, {"\\", {4, 3}}, {"H", {4, 4}}, {"<", {4, 5}}, {"A", {4, 6}}, {"F", {4, 7}},
    {"DEL", {5, 0}}, {"K", {5, 1}}, {"L", {5, 2}}, {"Á", {5, 3}}, {"RETURN", {5, 4}}, {"Ű", {5, 5}}, {"É", {5, 6}}, {"J", {5, 7}},
    {"B", {6, 0}}, {"C", {6, 1}}, {"X", {6, 2}}, {"SHIFT", {6, 3}}, {"N", {6, 4}}, {"LOCK", {6, 5}}, {"Y", {6, 6}}, {"V", {6, 7}},
    {"ALT", {7, 0}}, {",", {7, 1}}, {".", {7, 2}}, {"ESC", {7, 3}}, {"CTRL", {7, 4}}, {"SPACE", {7, 5}}, {"-", {7, 6}}, {"M", {7, 7}},
    {"INS", {8, 0}}, {"UP", {8, 1}}, {"DOWN", {8, 2}}, {"FIRE", {8, 3}}, {"ACCEL", {8, 4}}, {"RIGHT", {8, 5}}, {"LEFT", {8, 6}},
    {"ENTER", {5, 4}}, {"ESCAPE", {7, 3}}, {"CONTROL", {7, 4}},
    // Joystick 2, row 9 (named for the keyboard editor)
    {"JOY2_UP", {9, 1}}, {"JOY2_DOWN", {9, 2}}, {"JOY2_FIRE", {9, 3}}, {"JOY2_ACCEL", {9, 4}}, {"JOY2_RIGHT", {9, 5}}, {"JOY2_LEFT", {9, 6}},
    // The GameCard's joysticks 3 and 4
    {"JOY3_UP", {10, 0}}, {"JOY3_DOWN", {10, 1}}, {"JOY3_LEFT", {10, 2}}, {"JOY3_RIGHT", {10, 3}}, {"JOY3_FIRE", {10, 4}},
    {"JOY4_UP", {11, 0}}, {"JOY4_DOWN", {11, 1}}, {"JOY4_LEFT", {11, 2}}, {"JOY4_RIGHT", {11, 3}}, {"JOY4_FIRE", {11, 4}},
};

// Unshifted and shifted character of each key, matrix rows 0-7 in order.
constexpr CharKey kCharKeys[] = {
    {U'5', 0x35, {0, 0}, false}, {U'%', 0x25, {0, 0}, true}, {U'3', 0x33, {0, 1}, false}, {U'+', 0x2B, {0, 1}, true},
    {U'2', 0x32, {0, 2}, false}, {U'"', 0x22, {0, 2}, true}, {U'0', 0x30, {0, 3}, false}, {U'&', 0x26, {0, 3}, true},
    {U'6', 0x36, {0, 4}, false}, {U'/', 0x2F, {0, 4}, true}, {U'í', 0x92, {0, 5}, false}, {U'Í', 0x82, {0, 5}, true},
    {U'1', 0x31, {0, 6}, false}, {U'\'', 0x27, {0, 6}, true}, {U'4', 0x34, {0, 7}, false}, {U'!', 0x21, {0, 7}, true},

    {U'^', 0x5E, {1, 0}, false}, {U'~', 0x7E, {1, 0}, true}, {U'8', 0x38, {1, 1}, false}, {U'(', 0x28, {1, 1}, true},
    {U'9', 0x39, {1, 2}, false}, {U')', 0x29, {1, 2}, true}, {U'ü', 0x97, {1, 3}, false}, {U'Ü', 0x87, {1, 3}, true},
    {U'*', 0x2A, {1, 4}, false}, {U'#', 0x23, {1, 4}, true}, {U'ó', 0x93, {1, 5}, false}, {U'Ó', 0x83, {1, 5}, true},
    {U'ö', 0x94, {1, 6}, false}, {U'Ö', 0x84, {1, 6}, true}, {U'7', 0x37, {1, 7}, false}, {U'=', 0x3D, {1, 7}, true},

    {U't', 0x74, {2, 0}, false}, {U'T', 0x54, {2, 0}, true}, {U'e', 0x65, {2, 1}, false}, {U'E', 0x45, {2, 1}, true},
    {U'w', 0x77, {2, 2}, false}, {U'W', 0x57, {2, 2}, true}, {U';', 0x3B, {2, 3}, false}, {U'$', 0x24, {2, 3}, true},
    {U'z', 0x7A, {2, 4}, false}, {U'Z', 0x5A, {2, 4}, true}, {U'@', 0x40, {2, 5}, false}, {U'`', 0x60, {2, 5}, true},
    {U'q', 0x71, {2, 6}, false}, {U'Q', 0x51, {2, 6}, true}, {U'r', 0x72, {2, 7}, false}, {U'R', 0x52, {2, 7}, true},

    {U']', 0x5D, {3, 0}, false}, {U'}', 0x7D, {3, 0}, true}, {U'i', 0x69, {3, 1}, false}, {U'I', 0x49, {3, 1}, true},
    {U'o', 0x6F, {3, 2}, false}, {U'O', 0x4F, {3, 2}, true}, {U'ő', 0x95, {3, 3}, false}, {U'Ő', 0x85, {3, 3}, true},
    {U'[', 0x5B, {3, 4}, false}, {U'{', 0x7B, {3, 4}, true}, {U'ú', 0x96, {3, 5}, false}, {U'Ú', 0x86, {3, 5}, true},
    {U'p', 0x70, {3, 6}, false}, {U'P', 0x50, {3, 6}, true}, {U'u', 0x75, {3, 7}, false}, {U'U', 0x55, {3, 7}, true},

    {U'g', 0x67, {4, 0}, false}, {U'G', 0x47, {4, 0}, true}, {U'd', 0x64, {4, 1}, false}, {U'D', 0x44, {4, 1}, true},
    {U's', 0x73, {4, 2}, false}, {U'S', 0x53, {4, 2}, true}, {U'\\', 0x5C, {4, 3}, false}, {U'|', 0x7C, {4, 3}, true},
    {U'h', 0x68, {4, 4}, false}, {U'H', 0x48, {4, 4}, true}, {U'<', 0x3C, {4, 5}, false}, {U'>', 0x3E, {4, 5}, true},
    {U'a', 0x61, {4, 6}, false}, {U'A', 0x41, {4, 6}, true}, {U'f', 0x66, {4, 7}, false}, {U'F', 0x46, {4, 7}, true},

    {U'k', 0x6B, {5, 1}, false}, {U'K', 0x4B, {5, 1}, true}, {U'l', 0x6C, {5, 2}, false}, {U'L', 0x4C, {5, 2}, true},
    {U'á', 0x90, {5, 3}, false}, {U'Á', 0x80, {5, 3}, true}, {U'\r', 0x0D, {5, 4}, false}, {U'ű', 0x98, {5, 5}, false},
    {U'Ű', 0x88, {5, 5}, true}, {U'é', 0x91, {5, 6}, false}, {U'É', 0x81, {5, 6}, true}, {U'j', 0x6A, {5, 7}, false},
    {U'J', 0x4A, {5, 7}, true},

    {U'b', 0x62, {6, 0}, false}, {U'B', 0x42, {6, 0}, true}, {U'c', 0x63, {6, 1}, false}, {U'C', 0x43, {6, 1}, true},
    {U'x', 0x78, {6, 2}, false}, {U'X', 0x58, {6, 2}, true}, {U'n', 0x6E, {6, 4}, false}, {U'N', 0x4E, {6, 4}, true},
    {U'y', 0x79, {6, 6}, false}, {U'Y', 0x59, {6, 6}, true}, {U'v', 0x76, {6, 7}, false}, {U'V', 0x56, {6, 7}, true},

    {U',', 0x2C, {7, 1}, false}, {U'?', 0x3F, {7, 1}, true}, {U'.', 0x2E, {7, 2}, false}, {U':', 0x3A, {7, 2}, true},
    {U'\x1B', 0x1B, {7, 3}, false}, {U' ', 0x20, {7, 5}, false}, {U'-', 0x2D, {7, 6}, false}, {U'_', 0x5F, {7, 6}, true},
    {U'm', 0x6D, {7, 7}, false}, {U'M', 0x4D, {7, 7}, true},
};

}  // namespace

std::span<const NamedKey> named_keys() { return kNamedKeys; }

std::span<const CharKey> char_keys() { return kCharKeys; }

std::optional<MatrixKey> find_key(std::string_view name) {
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    for (const NamedKey& entry : kNamedKeys) {
        if (entry.name.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            same = upper(entry.name[i]) == upper(name[i]);
        }
        if (same) {
            return entry.key;
        }
    }
    // Lower-case accented letters ("á") name the same key as their capitals
    const auto text = decode_utf8(name);
    if (text && text->size() == 1 && is_letter((*text)[0])) {
        return find_char((*text)[0])->key;
    }
    return std::nullopt;
}

const CharKey* find_char(char32_t ch) {
    for (const CharKey& entry : kCharKeys) {
        if (entry.ch == ch) {
            return &entry;
        }
    }
    return nullptr;
}

bool is_letter(char32_t ch) {
    if ((ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z')) {
        return true;
    }
    const CharKey* entry = find_char(ch);
    return entry && entry->code >= 0x80;
}

std::optional<std::u32string> decode_utf8(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) {
            return std::nullopt;
        }
        char32_t ch = length == 1 ? lead : lead & (0x7Fu >> length);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next >> 6) != 0x2) {
                return std::nullopt;
            }
            ch = (ch << 6) | (next & 0x3Fu);
        }
        out.push_back(ch);
        i += length;
    }
    return out;
}

}  // namespace freetvc
