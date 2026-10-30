// SPDX-License-Identifier: GPL-3.0-or-later
// Host keyboard and gamepads to the TVC matrix (design/machine.md "Host input").
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/input/keyboard.hpp"
#include "frontend/sdl/keymap.hpp"

namespace freetvc {
class Machine;
}

namespace freetvc::sdl {

// Symbolic: the character the host key gives under the host's layout is typed on the TVC, with SHIFT forced as needed;
// keys that type no character (RETURN, the cursor keys, F1-F8...) follow the keymap. Positional: every key follows the
// keymap, by default the TVC key with the same legend on a Hungarian PC keyboard (the TVC layout is one).
enum class KeyMapping { Symbolic, Positional };
std::optional<KeyMapping> parse_key_mapping(std::string_view name);

class HostInput {
public:
    explicit HostInput(KeyMapping mapping) : mapping_(mapping), keymap_(builtin_keymap()) {}
    ~HostInput();
    HostInput(const HostInput&) = delete;
    HostInput& operator=(const HostInput&) = delete;

    void handle_event(const SDL_Event& event);
    // Lets go of every held key; update() then releases them on the matrix.
    void release_all() { held_.clear(); }
    // After a power-on or restored state: the matrix is not what update() last wrote, and the clock moved.
    void machine_replaced() {
        held_.clear();
        applied_.fill(false);
        waiting_ = false;
    }
    KeyMapping mapping() const { return mapping_; }
    void set_mapping(KeyMapping mapping) {
        mapping_ = mapping;
        held_.clear();
    }
    const Keymap& keymap() const { return keymap_; }
    void set_keymap(Keymap keymap) {
        keymap_ = std::move(keymap);
        held_.clear();
    }
    // Brings the matrix up to date; a SHIFT change gets a frame period of its own, after releases and before presses,
    // so the once-a-frame scan never sees a key with the wrong SHIFT.
    void update(Machine& machine);

private:
    struct HeldKey {
        SDL_Scancode scancode;
        MatrixKey key;
        int shift;  // -1 = leave SHIFT to the user, 0/1 = force it while this key is down
        std::optional<std::uint64_t> applied_at;  // machine ticks when it reached the matrix
        bool released = false;                    // let go: it leaves once it has been down for a frame
    };
    struct Pad {
        SDL_Gamepad* gamepad;
        SDL_JoystickID id;
        unsigned joystick;
        std::array<bool, 7> lines;  // indexed by JoyLine
    };

    void key_down(const SDL_KeyboardEvent& key);
    std::optional<HeldKey> map_symbolic(const SDL_KeyboardEvent& key) const;
    std::optional<HeldKey> map_keymap(SDL_Scancode scancode) const;
    void add_gamepad(SDL_JoystickID id);
    void remove_gamepad(SDL_JoystickID id);
    void poll_gamepad(SDL_JoystickID id);
    std::array<bool, kMatrixKeys> wanted(const std::vector<bool>& active) const;

    KeyMapping mapping_;
    Keymap keymap_;
    std::vector<HeldKey> held_;  // in press order, a key's earlier taps included; the newest forced SHIFT wins
    std::vector<Pad> pads_;
    std::array<bool, kMatrixKeys> applied_{};
    std::array<std::optional<std::uint64_t>, kMatrixKeys> up_at_{};  // machine ticks when each key last left the matrix
    std::optional<std::uint64_t> pressed_at_;  // machine ticks of the latest new key press (SHIFT aside)
    bool waiting_ = false;
    std::uint64_t step_tick_ = 0;
};

}  // namespace freetvc::sdl
