// SPDX-License-Identifier: GPL-3.0-or-later
// HostInput end to end with SDL's dummy video driver and its built-in US keymap: synthetic key events must type the
// same characters on the TVC (OS screen copy at 0100h); virtual gamepads must drive joystick rows 8 and 9, and 10-11.
// usage: host_input_test <rom dir>; exit 77 (CTest skip) when the ROM files are missing.
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/sdl/host_input.hpp"

namespace {

constexpr int kExitSkip = 77;
constexpr unsigned kColumns = 32;  // BASIC's 4-colour mode
constexpr unsigned kLineRow = 6;   // first row after "ok"

int g_failures = 0;

void run_frames(freetvc::Machine& m, freetvc::sdl::HostInput& input, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i) {
        input.update(m);
        m.run({.max_ticks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
    }
}

void send_key(freetvc::sdl::HostInput& input, SDL_Scancode scancode, bool down, SDL_Keymod mod) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.mod = mod;
    event.key.down = down;
    input.handle_event(event);
}

// A user's stroke: SHIFT first when the host layout needs it, then the key, at human pace
void stroke(freetvc::Machine& m, freetvc::sdl::HostInput& input, SDL_Scancode scancode, bool shift) {
    const SDL_Keymod mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
    if (shift) {
        send_key(input, SDL_SCANCODE_LSHIFT, true, mod);
        run_frames(m, input, 2);
    }
    send_key(input, scancode, true, mod);
    run_frames(m, input, 3);
    send_key(input, scancode, false, mod);
    run_frames(m, input, 2);
    if (shift) {
        send_key(input, SDL_SCANCODE_LSHIFT, false, SDL_KMOD_NONE);
        run_frames(m, input, 2);
    }
}

void check_line(const freetvc::Machine& m, const std::vector<std::uint8_t>& expected, const char* what) {
    unsigned wrong = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::uint8_t got = m.ram()[0x100 + (kLineRow + i / kColumns) * 64 + i % kColumns];
        if (got != expected[i] && ++wrong <= 8) {
            std::printf("  %s: char %zu screen %02Xh, expected %02Xh\n", what, i, got, expected[i]);
        }
    }
    std::printf("%s: %zu characters, %u wrong\n", what, expected.size(), wrong);
    g_failures += wrong != 0;
}

// US "=" is unshifted, the TVC's is SHIFT+7: SHIFT must reach the matrix a frame before the key and leave a frame after
void test_shift_order() {
    freetvc::Machine m;
    freetvc::sdl::HostInput input(freetvc::sdl::KeyMapping::Symbolic);
    const auto state = [&] { return (m.key_down(6, 3) ? 1u : 0u) | (m.key_down(1, 7) ? 2u : 0u); };
    std::vector<unsigned> seen;
    send_key(input, SDL_SCANCODE_EQUALS, true, SDL_KMOD_NONE);
    input.update(m);
    seen.push_back(state());
    input.update(m);  // same frame: the key must wait
    seen.push_back(state());
    run_frames(m, input, 1);
    input.update(m);
    seen.push_back(state());
    send_key(input, SDL_SCANCODE_EQUALS, false, SDL_KMOD_NONE);
    input.update(m);
    seen.push_back(state());
    run_frames(m, input, 1);
    input.update(m);
    seen.push_back(state());
    run_frames(m, input, 1);
    input.update(m);
    seen.push_back(state());
    // The key stays down for a frame of machine time, then leaves a frame before SHIFT
    const std::vector<unsigned> expected{1, 1, 3, 3, 1, 0};
    const bool ok = seen == expected;
    std::printf("shift order: %u %u %u %u %u %u %s\n", seen[0], seen[1], seen[2], seen[3], seen[4], seen[5], ok ? "ok" : "WRONG");
    g_failures += !ok;
}

// Host key taps shorter than a frame (a paste tool, a busy GUI frame): every tap must reach the matrix for at least a
// frame of machine time, with the key up for a frame between two taps, or the ROM's keyboard scan misses it
void test_quick_taps() {
    const auto frame = [](freetvc::Machine& m) { m.run({.max_ticks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState}); };
    const auto tap = [](freetvc::sdl::HostInput& input, SDL_Scancode scancode) {
        send_key(input, scancode, true, SDL_KMOD_NONE);
        send_key(input, scancode, false, SDL_KMOD_NONE);
    };
    struct Case {
        const char* name;
        std::vector<int> steps;  // 1 = a tap of Q, 2 = of W, 0 = an update (Q + 2 W recorded), -1 = a frame of machine time
        std::vector<unsigned> expected;
    };
    const std::vector<Case> cases{
        {"tap between updates", {1, 0, -1, 0}, {1, 0}},
        {"two taps between updates", {1, 1, 0, -1, 0, -1, 0, -1, 0}, {1, 0, 1, 0}},
        {"tap as the last one leaves", {1, 0, -1, 0, 1, 0, -1, 0, -1, 0}, {1, 0, 0, 1, 0}},
        {"release in the press's frame", {1, 0, 0, -1, 0}, {1, 1, 0}},
        {"two keys between updates", {1, 2, 0, -1, 0, -1, 0}, {1, 2, 0}},
        {"a repeated tap keeps its place", {1, 1, 2, 0, -1, 0, -1, 0, -1, 0, -1, 0}, {1, 0, 1, 2, 0}},
    };
    for (const Case& c : cases) {
        freetvc::Machine m;
        freetvc::sdl::HostInput input(freetvc::sdl::KeyMapping::Symbolic);
        std::vector<unsigned> seen;
        for (int step : c.steps) {
            if (step > 0) {
                tap(input, step == 1 ? SDL_SCANCODE_Q : SDL_SCANCODE_W);
            } else if (step == 0) {
                input.update(m);
                seen.push_back((m.key_down(2, 6) ? 1u : 0u) | (m.key_down(2, 2) ? 2u : 0u));
            } else {
                frame(m);
            }
        }
        const bool ok = seen == c.expected;
        std::printf("%s:", c.name);
        for (unsigned v : seen) {
            std::printf(" %u", v);
        }
        std::printf(" %s\n", ok ? "ok" : "WRONG");
        g_failures += !ok;
    }
}

void test_gamepads() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    const SDL_JoystickID first = SDL_AttachVirtualJoystick(&desc);
    const SDL_JoystickID second = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick* pad1 = SDL_OpenJoystick(first);
    SDL_Joystick* pad2 = SDL_OpenJoystick(second);
    if (!pad1 || !pad2) {
        std::printf("FAIL virtual gamepads: %s\n", SDL_GetError());
        ++g_failures;
        return;
    }

    freetvc::Machine m;
    freetvc::sdl::HostInput input(freetvc::sdl::KeyMapping::Symbolic);
    const auto pump = [&] {
        SDL_UpdateJoysticks();
        SDL_PumpEvents();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            input.handle_event(event);
        }
        input.update(m);
    };
    pump();
    SDL_SetJoystickVirtualButton(pad1, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_SetJoystickVirtualButton(pad1, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualAxis(pad2, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualButton(pad2, SDL_GAMEPAD_BUTTON_EAST, true);
    pump();
    const bool pressed = m.key_down(8, 1) && m.key_down(8, 3) && m.key_down(9, 6) && m.key_down(9, 4) &&
                         !m.key_down(8, 6) && !m.key_down(9, 1);
    SDL_SetJoystickVirtualButton(pad1, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualAxis(pad2, SDL_GAMEPAD_AXIS_LEFTX, 0);
    pump();
    const bool released = !m.key_down(8, 1) && m.key_down(8, 3) && !m.key_down(9, 6);
    std::printf("gamepads: pressed %s, released %s\n", pressed ? "ok" : "WRONG", released ? "ok" : "WRONG");
    g_failures += !(pressed && released);

    // Gamepads 3 and 4 are the GameCard's joysticks 3 and 4: rows 10 and 11, b0 up, b2 left, b4 fire
    const SDL_JoystickID third = SDL_AttachVirtualJoystick(&desc);
    const SDL_JoystickID fourth = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick* pad3 = SDL_OpenJoystick(third);
    SDL_Joystick* pad4 = SDL_OpenJoystick(fourth);
    pump();
    SDL_SetJoystickVirtualButton(pad3, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_SetJoystickVirtualButton(pad4, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualAxis(pad4, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
    pump();
    const bool extra = pad3 && pad4 && m.key_down(10, 0) && m.key_down(11, 4) && m.key_down(11, 2) && !m.key_down(10, 4);
    std::printf("gamepads 3-4: %s\n", extra ? "ok" : "WRONG");
    g_failures += !extra;
    SDL_CloseJoystick(pad3);
    SDL_CloseJoystick(pad4);
    SDL_DetachVirtualJoystick(third);
    SDL_DetachVirtualJoystick(fourth);

    SDL_CloseJoystick(pad1);
    SDL_CloseJoystick(pad2);
    SDL_DetachVirtualJoystick(first);
    SDL_DetachVirtualJoystick(second);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: host_input_test <rom dir>\n");
        return 2;
    }
    std::string error;
    const auto config = freetvc::load_preset(*freetvc::find_preset("64k+-2.2"), argv[1], error);
    if (!config) {
        std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
        return kExitSkip;
    }
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::printf("FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    freetvc::Machine m(*config);
    freetvc::sdl::HostInput symbolic(freetvc::sdl::KeyMapping::Symbolic);
    run_frames(m, symbolic, 330);
    m.set_key(7, 5, true);  // SPACE past the logo, as in boot_test
    run_frames(m, symbolic, 5);
    m.set_key(7, 5, false);
    run_frames(m, symbolic, 65);

    // Every printable ASCII character as a US keyboard types it; then "x" and Backspace, which must leave no trace
    std::vector<std::uint8_t> expected;
    for (char32_t ch = 0x20; ch < 0x7F; ++ch) {
        SDL_Keymod mod = SDL_KMOD_NONE;
        const SDL_Scancode scancode = SDL_GetScancodeFromKey(ch, &mod);
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            std::printf("FAIL no US key for U+%04X\n", static_cast<unsigned>(ch));
            ++g_failures;
            continue;
        }
        stroke(m, symbolic, scancode, (mod & SDL_KMOD_SHIFT) != 0);
        expected.push_back(static_cast<std::uint8_t>(ch));
    }
    stroke(m, symbolic, SDL_SCANCODE_X, false);
    stroke(m, symbolic, SDL_SCANCODE_BACKSPACE, false);
    run_frames(m, symbolic, 10);
    check_line(m, expected, "symbolic");

    // Positional: the key at a Hungarian PC legend's place, whatever the host layout says
    freetvc::sdl::HostInput positional(freetvc::sdl::KeyMapping::Positional);
    const struct {
        SDL_Scancode scancode;
        bool shift;
        std::uint8_t code;
    } kPositional[] = {
        {SDL_SCANCODE_Y, false, 0x7A},  {SDL_SCANCODE_Z, true, 0x59},     {SDL_SCANCODE_GRAVE, false, 0x30},
        {SDL_SCANCODE_0, false, 0x94},  {SDL_SCANCODE_NONUSBACKSLASH, false, 0x92}, {SDL_SCANCODE_SEMICOLON, true, 0x81},
        {SDL_SCANCODE_SLASH, false, 0x2D}, {SDL_SCANCODE_F1, false, 0x40}, {SDL_SCANCODE_F8, true, 0x7E},
        {SDL_SCANCODE_3, true, 0x2B},
    };
    for (const auto& key : kPositional) {
        stroke(m, positional, key.scancode, key.shift);
        expected.push_back(key.code);
    }
    run_frames(m, positional, 10);
    check_line(m, expected, "symbolic + positional");

    test_shift_order();
    test_quick_taps();
    test_gamepads();
    SDL_Quit();
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
