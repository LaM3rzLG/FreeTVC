// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/host_input.hpp"

#include <algorithm>

#include "core/machine/machine.hpp"

namespace freetvc::sdl {

namespace {

constexpr Sint16 kStickThreshold = 16000;  // about half deflection

}  // namespace

std::optional<KeyMapping> parse_key_mapping(std::string_view name) {
    if (name == "symbolic") {
        return KeyMapping::Symbolic;
    }
    if (name == "positional") {
        return KeyMapping::Positional;
    }
    return std::nullopt;
}

HostInput::~HostInput() {
    for (const Pad& pad : pads_) {
        SDL_CloseGamepad(pad.gamepad);
    }
}

void HostInput::handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat) {
            key_down(event.key);
        }
        break;
    case SDL_EVENT_KEY_UP:
        for (HeldKey& held : held_) {
            held.released = held.released || held.scancode == event.key.scancode;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        held_.clear();
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        add_gamepad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        poll_gamepad(event.gbutton.which);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        poll_gamepad(event.gaxis.which);
        break;
    default:
        break;
    }
}

void HostInput::key_down(const SDL_KeyboardEvent& key) {
    std::optional<HeldKey> held;
    if (mapping_ == KeyMapping::Positional) {
        held = map_keymap(key.scancode);
    } else {
        // A key that types a character goes by that character; AltGr only composes characters, as before key layouts
        const SDL_Keycode code = SDL_GetKeyFromScancode(key.scancode, key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_MODE), false);
        const bool types = code != SDLK_UNKNOWN && !(code & SDLK_SCANCODE_MASK) && code >= 0x20 && code != 0x7F;
        held = types ? map_symbolic(key) : key.scancode == SDL_SCANCODE_RALT ? std::nullopt : map_keymap(key.scancode);
    }
    if (held) {
        std::erase_if(held_, [&](const HeldKey& old) { return old.scancode == key.scancode && !old.released; });
        held_.push_back(*held);  // an earlier tap of the same key stays until it has had its frame
    }
}

std::optional<HostInput::HeldKey> HostInput::map_symbolic(const SDL_KeyboardEvent& key) const {
    // Event keycodes ignore the modifiers by default, so ask for the character under SHIFT and AltGr ourselves
    const SDL_Keymod mod = key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_MODE);
    const SDL_Keycode code = SDL_GetKeyFromScancode(key.scancode, mod, false);
    if (code == SDLK_UNKNOWN || (code & SDLK_SCANCODE_MASK)) {
        return std::nullopt;
    }
    const char32_t ch = static_cast<char32_t>(code);
    const CharKey* entry = find_char(ch);
    if (!entry) {
        return std::nullopt;
    }
    // Letters keep the user's SHIFT so the TVC's LOCK still works; SPACE types the same either way
    const bool free_shift = is_letter(ch) || entry->key == kKeySpace;
    return HeldKey{.scancode = key.scancode, .key = entry->key, .shift = free_shift ? -1 : (entry->shift ? 1 : 0), .applied_at = {}, .released = false};
}

std::optional<HostInput::HeldKey> HostInput::map_keymap(SDL_Scancode scancode) const {
    if (const KeyBinding* binding = keymap_.find(scancode)) {
        return HeldKey{.scancode = scancode, .key = binding->key, .shift = binding->shift, .applied_at = {}, .released = false};
    }
    return std::nullopt;
}

// Gamepads 1-2 are the TVC's joysticks 1-2, gamepads 3-4 the GameCard's joysticks 3-4; more are ignored
void HostInput::add_gamepad(SDL_JoystickID id) {
    unsigned joystick = 1;
    while (joystick <= 4 && std::any_of(pads_.begin(), pads_.end(), [&](const Pad& p) { return p.joystick == joystick; })) {
        ++joystick;
    }
    if (joystick > 4 || std::any_of(pads_.begin(), pads_.end(), [&](const Pad& p) { return p.id == id; })) {
        return;
    }
    if (SDL_Gamepad* gamepad = SDL_OpenGamepad(id)) {
        pads_.push_back({gamepad, id, joystick, {}});
    }
}

void HostInput::remove_gamepad(SDL_JoystickID id) {
    for (const Pad& pad : pads_) {
        if (pad.id == id) {
            SDL_CloseGamepad(pad.gamepad);
        }
    }
    std::erase_if(pads_, [&](const Pad& pad) { return pad.id == id; });
}

void HostInput::poll_gamepad(SDL_JoystickID id) {
    for (Pad& pad : pads_) {
        if (pad.id != id) {
            continue;
        }
        const auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad.gamepad, b); };
        const Sint16 x = SDL_GetGamepadAxis(pad.gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 y = SDL_GetGamepadAxis(pad.gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        pad.lines[static_cast<unsigned>(JoyLine::Up)] = button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kStickThreshold;
        pad.lines[static_cast<unsigned>(JoyLine::Down)] = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kStickThreshold;
        pad.lines[static_cast<unsigned>(JoyLine::Left)] = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kStickThreshold;
        pad.lines[static_cast<unsigned>(JoyLine::Right)] = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kStickThreshold;
        pad.lines[static_cast<unsigned>(JoyLine::Fire)] = button(SDL_GAMEPAD_BUTTON_SOUTH);
        pad.lines[static_cast<unsigned>(JoyLine::Accel)] = button(SDL_GAMEPAD_BUTTON_EAST);
    }
}

std::array<bool, kMatrixKeys> HostInput::wanted(const std::vector<bool>& active) const {
    std::array<bool, kMatrixKeys> keys{};
    int forced = -1;
    for (std::size_t i = 0; i < held_.size(); ++i) {
        const HeldKey& held = held_[i];
        if (!active[i]) {
            continue;
        }
        keys[key_index(held.key)] = true;
        if (held.shift >= 0) {
            forced = held.shift;
        }
    }
    for (const Pad& pad : pads_) {
        for (unsigned line = 1; line < pad.lines.size(); ++line) {
            if (pad.lines[line]) {
                keys[key_index(joystick_key(pad.joystick, static_cast<JoyLine>(line)))] = true;
            }
        }
    }
    if (forced >= 0) {
        keys[key_index(kKeyShift)] = forced == 1;
    }
    return keys;
}

// Steps are a frame period of machine time apart, not frame flips: an unprogrammed CRTC never flips a frame
void HostInput::update(Machine& machine) {
    if (waiting_ && machine.ticks() < step_tick_ + kTStatesPerFrame * kTicksPerTState) {
        return;
    }
    waiting_ = false;
    // A key leaves the matrix only after a frame of machine time, so a quick tap still meets a keyboard scan
    const std::uint64_t frame = kTStatesPerFrame * kTicksPerTState;
    std::erase_if(held_, [&](const HeldKey& held) {
        return held.released && held.applied_at && machine.ticks() - *held.applied_at >= frame;
    });
    // The ROM takes one new key per keyboard scan, so new presses reach the matrix one per frame, in press order,
    // each once its key has been up for a frame
    const std::uint64_t now = machine.ticks();
    bool may_press = !pressed_at_ || now - *pressed_at_ >= frame;
    std::vector<bool> active(held_.size(), true);
    for (std::size_t i = 0; i < held_.size(); ++i) {
        const unsigned k = key_index(held_[i].key);
        if (held_[i].applied_at || held_[i].key == kKeyShift) {
            continue;
        }
        if (may_press && !applied_[k] && (!up_at_[k] || now - *up_at_[k] >= frame)) {
            may_press = false;
            continue;
        }
        may_press = false;
        active[i] = false;
    }
    const std::array<bool, kMatrixKeys> want = wanted(active);
    const unsigned shift = key_index(kKeyShift);
    bool releases = false;
    for (unsigned i = 0; i < kMatrixKeys; ++i) {
        releases = releases || (i != shift && applied_[i] && !want[i]);
    }
    const auto apply = [&](unsigned i) {
        if (applied_[i] != want[i]) {
            if (applied_[i]) {
                up_at_[i] = machine.ticks();
            } else if (i != key_index(kKeyShift)) {
                pressed_at_ = machine.ticks();
            }
            applied_[i] = want[i];
            machine.set_key(i / 8, i % 8, want[i]);
        }
    };
    const auto wait = [&] {
        waiting_ = true;
        step_tick_ = machine.ticks();
    };
    if (want[shift] == applied_[shift]) {
        for (unsigned i = 0; i < kMatrixKeys; ++i) {
            apply(i);
        }
    } else if (releases) {
        for (unsigned i = 0; i < kMatrixKeys; ++i) {
            if (i != shift && applied_[i] && !want[i]) {
                apply(i);
            }
        }
        wait();
    } else {
        apply(shift);
        wait();
    }
    for (std::size_t i = 0; i < held_.size(); ++i) {
        if (active[i] && !held_[i].applied_at && applied_[key_index(held_[i].key)]) {
            held_[i].applied_at = machine.ticks();
        }
    }
}

}  // namespace freetvc::sdl
