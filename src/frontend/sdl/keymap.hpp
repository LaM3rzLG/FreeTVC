// SPDX-License-Identifier: GPL-3.0-or-later
// Host key layouts: which host key presses which TVC key (design/ui.md "Keyboard editor"). `name = KEY [+shift|-shift]`
// lines in keymaps/<name>.keys; host keys by stable names (A, GRAVE, KP_8, RCTRL), TVC keys by their legend or word.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_scancode.h>

#include "core/input/keyboard.hpp"

namespace freetvc::sdl {

struct KeyBinding {
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    MatrixKey key;
    int shift = -1;  // -1 the user's SHIFT, 0/1 forced while the key is down
    bool operator==(const KeyBinding&) const = default;
};

struct Keymap {
    std::string name = "hungarian";
    std::vector<KeyBinding> bindings;  // one per host key
    bool operator==(const Keymap&) const = default;

    const KeyBinding* find(SDL_Scancode scancode) const;
    // Binds `scancode` to `key` (replacing what it pressed before); SDL_SCANCODE_UNKNOWN does nothing
    void bind(SDL_Scancode scancode, MatrixKey key, int shift = -1);
    // Every host key that presses `key`
    std::vector<SDL_Scancode> host_keys(MatrixKey key) const;
    void clear(MatrixKey key);
};

// Today's positional table and special keys: the Hungarian PC keyboard; data/keymaps/hungarian.keys says the same
Keymap builtin_keymap();

// The host keys a file may name: the stable name and a short label for keys that type no character
struct HostKeyName {
    SDL_Scancode scancode;
    std::string_view name;
    std::string_view label;  // empty: the label is the character the host layout gives
};
std::span<const HostKeyName> host_key_names();
std::optional<SDL_Scancode> find_host_key(std::string_view name);
std::string_view host_key_name(SDL_Scancode scancode);  // empty for keys without a name
// The TVC key's name for files: its unshifted legend or word, as core/input names it
std::string_view tvc_key_name(MatrixKey key);

// Applies "KEY = TVC" lines on top of keymap; ';' starts a comment. Returns "line N: ..." messages (msg()).
std::vector<std::string> parse_keymap(std::string_view text, Keymap& keymap);
std::optional<Keymap> load_keymap_file(const std::filesystem::path& path, std::string& error);
std::string keymap_text(const Keymap& keymap);  // the whole layout, every binding

struct KeymapEntry {
    std::string name;
    std::filesystem::path path;
};
std::vector<KeymapEntry> list_keymaps(const std::vector<std::filesystem::path>& dirs);

}  // namespace freetvc::sdl
