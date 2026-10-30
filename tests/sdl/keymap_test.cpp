// SPDX-License-Identifier: GPL-3.0-or-later
// Host key layouts: data/keymaps/hungarian.keys equals the built-in layout, us.keys loads, every host key and TVC key
// name round-trips, and bad lines are reported with their number.
// usage: keymap_test <data/keymaps directory> | keymap_test --print (the built-in layout as a file)
#include <cstdio>
#include <filesystem>
#include <string>

#include "frontend/sdl/keymap.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace freetvc::sdl;
    if (argc == 2 && std::string(argv[1]) == "--print") {
        std::printf("%s", keymap_text(builtin_keymap()).c_str());
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: keymap_test <data/keymaps directory>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::string error;
    const auto hungarian = load_keymap_file(dir / "hungarian.keys", error);
    check(hungarian.has_value(), "hungarian.keys loads: " + error);
    check(hungarian && *hungarian == builtin_keymap(), "hungarian.keys equals the built-in layout");
    const auto us = load_keymap_file(dir / "us.keys", error);
    check(us.has_value() && us->name == "us", "us.keys loads: " + error);
    check(list_keymaps({dir}).size() == 2, "two shipped layouts");

    for (const HostKeyName& h : host_key_names()) {
        check(find_host_key(h.name) == h.scancode && host_key_name(h.scancode) == h.name, "host key " + std::string(h.name));
    }
    Keymap round = builtin_keymap();
    round.name = "round";
    round.bind(SDL_SCANCODE_TAB, freetvc::MatrixKey{2, 3});  // ';', written as SEMICOLON
    round.bind(SDL_SCANCODE_KP_5, freetvc::joystick_key(2, freetvc::JoyLine::Accel));
    round.bind(SDL_SCANCODE_F9, freetvc::MatrixKey{1, 0});   // no name: left out of the file
    Keymap back;
    back.bindings.clear();
    check(parse_keymap(keymap_text(round), back).empty(), "keymap_text parses");
    round.bindings.pop_back();
    check(back == round, "keymap_text reads back");

    Keymap bad = builtin_keymap();
    const auto errors = parse_keymap("A = Q\nCAPS = LOCK\nB = Ŵ\nC\nDELETE = none\nX = DEL +shift\n", bad);
    check(errors.size() == 3 && errors[0].starts_with("line 2:") && errors[1].starts_with("line 3:") && errors[2].starts_with("line 4:"),
          "bad lines reported with their number");
    check(bad.find(SDL_SCANCODE_A)->key == freetvc::MatrixKey{2, 6} && !bad.find(SDL_SCANCODE_DELETE) &&
              bad.find(SDL_SCANCODE_X)->shift == 1,
          "good lines applied: a rebinding, an unbinding, a forced SHIFT");
    check(bad.host_keys(freetvc::kKeyDel).size() == 2, "two host keys on DEL");

    std::printf("%s\n", g_failures ? "keymap_test: failures" : "keymap_test: ok");
    return g_failures ? 1 : 0;
}
