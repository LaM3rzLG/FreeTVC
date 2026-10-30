// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/keymap.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/message.hpp"

namespace freetvc::sdl {

namespace {

constexpr HostKeyName kHostKeys[] = {
    {SDL_SCANCODE_A, "A", ""}, {SDL_SCANCODE_B, "B", ""}, {SDL_SCANCODE_C, "C", ""}, {SDL_SCANCODE_D, "D", ""},
    {SDL_SCANCODE_E, "E", ""}, {SDL_SCANCODE_F, "F", ""}, {SDL_SCANCODE_G, "G", ""}, {SDL_SCANCODE_H, "H", ""},
    {SDL_SCANCODE_I, "I", ""}, {SDL_SCANCODE_J, "J", ""}, {SDL_SCANCODE_K, "K", ""}, {SDL_SCANCODE_L, "L", ""},
    {SDL_SCANCODE_M, "M", ""}, {SDL_SCANCODE_N, "N", ""}, {SDL_SCANCODE_O, "O", ""}, {SDL_SCANCODE_P, "P", ""},
    {SDL_SCANCODE_Q, "Q", ""}, {SDL_SCANCODE_R, "R", ""}, {SDL_SCANCODE_S, "S", ""}, {SDL_SCANCODE_T, "T", ""},
    {SDL_SCANCODE_U, "U", ""}, {SDL_SCANCODE_V, "V", ""}, {SDL_SCANCODE_W, "W", ""}, {SDL_SCANCODE_X, "X", ""},
    {SDL_SCANCODE_Y, "Y", ""}, {SDL_SCANCODE_Z, "Z", ""},
    {SDL_SCANCODE_1, "1", ""}, {SDL_SCANCODE_2, "2", ""}, {SDL_SCANCODE_3, "3", ""}, {SDL_SCANCODE_4, "4", ""},
    {SDL_SCANCODE_5, "5", ""}, {SDL_SCANCODE_6, "6", ""}, {SDL_SCANCODE_7, "7", ""}, {SDL_SCANCODE_8, "8", ""},
    {SDL_SCANCODE_9, "9", ""}, {SDL_SCANCODE_0, "0", ""},
    {SDL_SCANCODE_GRAVE, "GRAVE", ""}, {SDL_SCANCODE_MINUS, "MINUS", ""}, {SDL_SCANCODE_EQUALS, "EQUALS", ""},
    {SDL_SCANCODE_LEFTBRACKET, "LEFTBRACKET", ""}, {SDL_SCANCODE_RIGHTBRACKET, "RIGHTBRACKET", ""},
    {SDL_SCANCODE_BACKSLASH, "BACKSLASH", ""}, {SDL_SCANCODE_NONUSHASH, "NONUSHASH", ""},
    {SDL_SCANCODE_SEMICOLON, "SEMICOLON", ""}, {SDL_SCANCODE_APOSTROPHE, "APOSTROPHE", ""},
    {SDL_SCANCODE_COMMA, "COMMA", ""}, {SDL_SCANCODE_PERIOD, "PERIOD", ""}, {SDL_SCANCODE_SLASH, "SLASH", ""},
    {SDL_SCANCODE_NONUSBACKSLASH, "NONUSBACKSLASH", ""}, {SDL_SCANCODE_SPACE, "SPACE", "Space"},
    {SDL_SCANCODE_RETURN, "RETURN", "Enter"}, {SDL_SCANCODE_ESCAPE, "ESCAPE", "Esc"}, {SDL_SCANCODE_BACKSPACE, "BACKSPACE", "Bksp"},
    {SDL_SCANCODE_TAB, "TAB", "Tab"}, {SDL_SCANCODE_CAPSLOCK, "CAPSLOCK", "Caps"},
    {SDL_SCANCODE_F1, "F1", "F1"}, {SDL_SCANCODE_F2, "F2", "F2"}, {SDL_SCANCODE_F3, "F3", "F3"}, {SDL_SCANCODE_F4, "F4", "F4"},
    {SDL_SCANCODE_F5, "F5", "F5"}, {SDL_SCANCODE_F6, "F6", "F6"}, {SDL_SCANCODE_F7, "F7", "F7"}, {SDL_SCANCODE_F8, "F8", "F8"},
    {SDL_SCANCODE_PRINTSCREEN, "PRINTSCREEN", "PrtSc"}, {SDL_SCANCODE_SCROLLLOCK, "SCROLLLOCK", "ScrLk"},
    {SDL_SCANCODE_INSERT, "INSERT", "Ins"}, {SDL_SCANCODE_HOME, "HOME", "Home"}, {SDL_SCANCODE_PAGEUP, "PAGEUP", "PgUp"},
    {SDL_SCANCODE_DELETE, "DELETE", "Del"}, {SDL_SCANCODE_END, "END", "End"}, {SDL_SCANCODE_PAGEDOWN, "PAGEDOWN", "PgDn"},
    {SDL_SCANCODE_RIGHT, "RIGHT", "Right"}, {SDL_SCANCODE_LEFT, "LEFT", "Left"}, {SDL_SCANCODE_DOWN, "DOWN", "Down"},
    {SDL_SCANCODE_UP, "UP", "Up"}, {SDL_SCANCODE_NUMLOCKCLEAR, "NUMLOCK", "Num"},
    {SDL_SCANCODE_KP_DIVIDE, "KP_DIVIDE", "Kp/"}, {SDL_SCANCODE_KP_MULTIPLY, "KP_MULTIPLY", "Kp*"},
    {SDL_SCANCODE_KP_MINUS, "KP_MINUS", "Kp-"}, {SDL_SCANCODE_KP_PLUS, "KP_PLUS", "Kp+"}, {SDL_SCANCODE_KP_ENTER, "KP_ENTER", "KpEnter"},
    {SDL_SCANCODE_KP_1, "KP_1", "Kp1"}, {SDL_SCANCODE_KP_2, "KP_2", "Kp2"}, {SDL_SCANCODE_KP_3, "KP_3", "Kp3"},
    {SDL_SCANCODE_KP_4, "KP_4", "Kp4"}, {SDL_SCANCODE_KP_5, "KP_5", "Kp5"}, {SDL_SCANCODE_KP_6, "KP_6", "Kp6"},
    {SDL_SCANCODE_KP_7, "KP_7", "Kp7"}, {SDL_SCANCODE_KP_8, "KP_8", "Kp8"}, {SDL_SCANCODE_KP_9, "KP_9", "Kp9"},
    {SDL_SCANCODE_KP_0, "KP_0", "Kp0"}, {SDL_SCANCODE_KP_PERIOD, "KP_PERIOD", "Kp."},
    {SDL_SCANCODE_LCTRL, "LCTRL", "LCtrl"}, {SDL_SCANCODE_LSHIFT, "LSHIFT", "LShift"}, {SDL_SCANCODE_LALT, "LALT", "LAlt"},
    {SDL_SCANCODE_LGUI, "LGUI", "LWin"}, {SDL_SCANCODE_RCTRL, "RCTRL", "RCtrl"}, {SDL_SCANCODE_RSHIFT, "RSHIFT", "RShift"},
    {SDL_SCANCODE_RALT, "RALT", "AltGr"}, {SDL_SCANCODE_RGUI, "RGUI", "RWin"}, {SDL_SCANCODE_APPLICATION, "MENU", "Menu"},
};

// The TVC keys whose legend cannot stand in a file (';' starts a comment)
struct TvcAlias {
    std::string_view name;
    MatrixKey key;
};
constexpr TvcAlias kTvcAliases[] = {{"SEMICOLON", {2, 3}}};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

std::optional<MatrixKey> find_tvc_key(std::string_view name) {
    for (const TvcAlias& alias : kTvcAliases) {
        if (alias.name == name) {
            return alias.key;
        }
    }
    return find_key(name);
}

}  // namespace

const KeyBinding* Keymap::find(SDL_Scancode scancode) const {
    const auto it = std::find_if(bindings.begin(), bindings.end(), [&](const KeyBinding& b) { return b.scancode == scancode; });
    return it == bindings.end() ? nullptr : &*it;
}

void Keymap::bind(SDL_Scancode scancode, MatrixKey key, int shift) {
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    std::erase_if(bindings, [&](const KeyBinding& b) { return b.scancode == scancode; });
    bindings.push_back({scancode, key, shift});
}

std::vector<SDL_Scancode> Keymap::host_keys(MatrixKey key) const {
    std::vector<SDL_Scancode> keys;
    for (const KeyBinding& b : bindings) {
        if (b.key == key) {
            keys.push_back(b.scancode);
        }
    }
    return keys;
}

void Keymap::clear(MatrixKey key) {
    std::erase_if(bindings, [&](const KeyBinding& b) { return b.key == key; });
}

Keymap builtin_keymap() {
    // Hungarian PC legends: 0 left of 1, Ö Ü Ó after 9, Ő Ú after P, É Á Ű after L, Í left of Y, "-" on the slash key.
    // The TVC keys a PC lacks sit on F1-F8 in their TVC order. The cursor keys are joystick 1, the keypad joystick 2.
    Keymap k;
    const auto add = [&](SDL_Scancode s, MatrixKey key, int shift = -1) { k.bindings.push_back({s, key, shift}); };
    add(SDL_SCANCODE_GRAVE, {0, 3});
    add(SDL_SCANCODE_1, {0, 6});
    add(SDL_SCANCODE_2, {0, 2});
    add(SDL_SCANCODE_3, {0, 1});
    add(SDL_SCANCODE_4, {0, 7});
    add(SDL_SCANCODE_5, {0, 0});
    add(SDL_SCANCODE_6, {0, 4});
    add(SDL_SCANCODE_7, {1, 7});
    add(SDL_SCANCODE_8, {1, 1});
    add(SDL_SCANCODE_9, {1, 2});
    add(SDL_SCANCODE_0, {1, 6});
    add(SDL_SCANCODE_MINUS, {1, 3});
    add(SDL_SCANCODE_EQUALS, {1, 5});
    add(SDL_SCANCODE_Q, {2, 6});
    add(SDL_SCANCODE_W, {2, 2});
    add(SDL_SCANCODE_E, {2, 1});
    add(SDL_SCANCODE_R, {2, 7});
    add(SDL_SCANCODE_T, {2, 0});
    add(SDL_SCANCODE_Y, {2, 4});
    add(SDL_SCANCODE_U, {3, 7});
    add(SDL_SCANCODE_I, {3, 1});
    add(SDL_SCANCODE_O, {3, 2});
    add(SDL_SCANCODE_P, {3, 6});
    add(SDL_SCANCODE_LEFTBRACKET, {3, 3});
    add(SDL_SCANCODE_RIGHTBRACKET, {3, 5});
    add(SDL_SCANCODE_BACKSLASH, {5, 5});
    add(SDL_SCANCODE_NONUSHASH, {5, 5});
    add(SDL_SCANCODE_A, {4, 6});
    add(SDL_SCANCODE_S, {4, 2});
    add(SDL_SCANCODE_D, {4, 1});
    add(SDL_SCANCODE_F, {4, 7});
    add(SDL_SCANCODE_G, {4, 0});
    add(SDL_SCANCODE_H, {4, 4});
    add(SDL_SCANCODE_J, {5, 7});
    add(SDL_SCANCODE_K, {5, 1});
    add(SDL_SCANCODE_L, {5, 2});
    add(SDL_SCANCODE_SEMICOLON, {5, 6});
    add(SDL_SCANCODE_APOSTROPHE, {5, 3});
    add(SDL_SCANCODE_NONUSBACKSLASH, {0, 5});
    add(SDL_SCANCODE_Z, {6, 6});
    add(SDL_SCANCODE_X, {6, 2});
    add(SDL_SCANCODE_C, {6, 1});
    add(SDL_SCANCODE_V, {6, 7});
    add(SDL_SCANCODE_B, {6, 0});
    add(SDL_SCANCODE_N, {6, 4});
    add(SDL_SCANCODE_M, {7, 7});
    add(SDL_SCANCODE_COMMA, {7, 1});
    add(SDL_SCANCODE_PERIOD, {7, 2});
    add(SDL_SCANCODE_SLASH, {7, 6});
    add(SDL_SCANCODE_SPACE, kKeySpace);
    add(SDL_SCANCODE_RALT, kKeyAlt);
    add(SDL_SCANCODE_F1, {2, 5});
    add(SDL_SCANCODE_F2, {2, 3});
    add(SDL_SCANCODE_F3, {4, 5});
    add(SDL_SCANCODE_F4, {4, 3});
    add(SDL_SCANCODE_F5, {3, 4});
    add(SDL_SCANCODE_F6, {3, 0});
    add(SDL_SCANCODE_F7, {1, 4});
    add(SDL_SCANCODE_F8, {1, 0});
    add(SDL_SCANCODE_RETURN, kKeyReturn);
    add(SDL_SCANCODE_KP_ENTER, kKeyReturn);
    add(SDL_SCANCODE_BACKSPACE, kKeyDel, 0);  // DEL deletes left of the cursor
    add(SDL_SCANCODE_DELETE, kKeyDel, 1);     // SHIFT+DEL deletes under the cursor
    add(SDL_SCANCODE_ESCAPE, kKeyEsc);
    add(SDL_SCANCODE_INSERT, kKeyIns);
    add(SDL_SCANCODE_LSHIFT, kKeyShift);
    add(SDL_SCANCODE_RSHIFT, kKeyShift);
    add(SDL_SCANCODE_CAPSLOCK, kKeyLock);
    add(SDL_SCANCODE_LCTRL, kKeyCtrl);
    add(SDL_SCANCODE_LALT, kKeyAlt);
    add(SDL_SCANCODE_UP, joystick_key(1, JoyLine::Up));
    add(SDL_SCANCODE_DOWN, joystick_key(1, JoyLine::Down));
    add(SDL_SCANCODE_LEFT, joystick_key(1, JoyLine::Left));
    add(SDL_SCANCODE_RIGHT, joystick_key(1, JoyLine::Right));
    add(SDL_SCANCODE_RCTRL, joystick_key(1, JoyLine::Fire));
    add(SDL_SCANCODE_KP_8, joystick_key(2, JoyLine::Up));
    add(SDL_SCANCODE_KP_2, joystick_key(2, JoyLine::Down));
    add(SDL_SCANCODE_KP_4, joystick_key(2, JoyLine::Left));
    add(SDL_SCANCODE_KP_6, joystick_key(2, JoyLine::Right));
    add(SDL_SCANCODE_KP_0, joystick_key(2, JoyLine::Fire));
    return k;
}

std::span<const HostKeyName> host_key_names() { return kHostKeys; }

std::optional<SDL_Scancode> find_host_key(std::string_view name) {
    for (const HostKeyName& h : kHostKeys) {
        if (h.name == name) {
            return h.scancode;
        }
    }
    return std::nullopt;
}

std::string_view host_key_name(SDL_Scancode scancode) {
    for (const HostKeyName& h : kHostKeys) {
        if (h.scancode == scancode) {
            return h.name;
        }
    }
    return {};
}

std::string_view tvc_key_name(MatrixKey key) {
    for (const TvcAlias& alias : kTvcAliases) {
        if (alias.key == key) {
            return alias.name;
        }
    }
    for (const NamedKey& named : named_keys()) {
        if (named.key == key) {
            return named.name;
        }
    }
    return {};
}

std::vector<std::string> parse_keymap(std::string_view text, Keymap& keymap) {
    std::vector<std::string> errors;
    int number = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++number;
        line = trim(line.substr(0, line.find(';')));
        if (line.empty()) {
            continue;
        }
        const auto fail = [&](const std::string& why) { errors.push_back(msg("line %d: %s", number, why.c_str())); };
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            fail(msg("expected key = value"));
            continue;
        }
        const std::string_view left = trim(line.substr(0, equals));
        std::string_view right = trim(line.substr(equals + 1));
        if (left == "name") {
            if (right.empty()) {
                fail(msg("expected a value"));
            } else {
                keymap.name = std::string(right);
            }
            continue;
        }
        const auto host = find_host_key(left);
        if (!host) {
            fail(msg("unknown host key '%s'", std::string(left).c_str()));
            continue;
        }
        if (right == "none") {
            std::erase_if(keymap.bindings, [&](const KeyBinding& b) { return b.scancode == *host; });
            continue;
        }
        int shift = -1;
        if (const auto space = right.find_last_of(" \t"); space != std::string_view::npos) {
            const std::string_view suffix = right.substr(space + 1);
            if (suffix == "+shift" || suffix == "-shift") {
                shift = suffix == "+shift" ? 1 : 0;
                right = trim(right.substr(0, space));
            }
        }
        const auto key = find_tvc_key(right);
        if (!key) {
            fail(msg("unknown TVC key '%s'", std::string(right).c_str()));
            continue;
        }
        keymap.bind(*host, *key, shift);
    }
    return errors;
}

std::optional<Keymap> load_keymap_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = msg("cannot read '%s'", path.string().c_str());
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    Keymap keymap;
    keymap.bindings.clear();
    const auto errors = parse_keymap(text.str(), keymap);
    if (!errors.empty()) {
        error = msg("%s %s", path.filename().string().c_str(), errors.front().c_str());
        return std::nullopt;
    }
    return keymap;
}

std::string keymap_text(const Keymap& keymap) {
    std::string text = "name = " + keymap.name + "\n";
    for (const KeyBinding& b : keymap.bindings) {
        const std::string_view host = host_key_name(b.scancode);
        const std::string_view tvc = tvc_key_name(b.key);
        if (host.empty() || tvc.empty()) {
            continue;
        }
        text += std::string(host) + " = " + std::string(tvc) + (b.shift == 1 ? " +shift" : b.shift == 0 ? " -shift" : "") + "\n";
    }
    return text;
}

std::vector<KeymapEntry> list_keymaps(const std::vector<std::filesystem::path>& dirs) {
    std::map<std::string, std::filesystem::path> found;
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".keys" && entry.is_regular_file(ec)) {
                found.try_emplace(entry.path().stem().string(), entry.path());
            }
        }
    }
    std::vector<KeymapEntry> keymaps;
    for (auto& [name, path] : found) {
        keymaps.push_back({name, path});
    }
    return keymaps;
}

}  // namespace freetvc::sdl
