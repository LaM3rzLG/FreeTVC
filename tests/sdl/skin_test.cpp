// SPDX-License-Identifier: GPL-3.0-or-later
// Skin parser and files: data/skins/bronze.skin must equal the built-in skin, every value form must parse, and
// bad lines must be reported with their line number.
// usage: skin_test <data/skins directory>
#include <cstdio>
#include <filesystem>
#include <string>

#include "frontend/sdl/skin.hpp"

namespace {

using freetvc::sdl::Colour;
using freetvc::sdl::Role;
using freetvc::sdl::Skin;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: skin_test <data/skins directory>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::string error;

    const auto bronze = freetvc::sdl::load_skin_file(dir / "bronze.skin", error);
    check(bronze.has_value(), ("bronze.skin loads: " + error).c_str());
    check(bronze && *bronze == Skin{}, "bronze.skin equals the built-in skin");

    const auto deck = freetvc::sdl::load_skin_file(dir / "deck.skin", error);
    check(deck.has_value(), ("deck.skin loads: " + error).c_str());
    if (deck) {
        Skin expected;
        expected.name = "deck";
        expected.frame = {0xcf, 0xc7, 0xb6};
        expected.deck = {0xe2, 0xdb, 0xcd};
        expected.groove = {0x8a, 0x7d, 0x6c};
        expected.rib = {0xcb, 0xc2, 0xb0};
        expected.key = {0xbf, 0xb5, 0xa2};
        expected.key_hover = {0xb2, 0xa8, 0x94};
        expected.key_active = {0xa5, 0x9a, 0x85};
        expected.label = {0x2b, 0x25, 0x20};
        expected.muted = {0x6f, 0x63, 0x54};
        expected.panel_light = true;
        expected.putty = {0xef, 0xe9, 0xde};
        for (unsigned r = 0; r < freetvc::sdl::kRoles; ++r) {
            const unsigned colours[] = {2, 4, 3, 5, 1, 6};  // red green magenta cyan blue yellow
            const Colour dark = freetvc::sdl::tvc_role_colour(colours[r]).on_putty;
            expected.roles[r] = {dark, dark};
        }
        expected.roles[static_cast<std::size_t>(freetvc::sdl::Role::Notice)] = {{0x6e, 0x66, 0x00}, {0x6e, 0x66, 0x00}};
        expected.screen_margin = 24.0f;
        check(*deck == expected, "deck.skin changes only its own keys");
    }

    Skin skin;
    auto errors = freetvc::sdl::parse_skin(
        "colour.frame = #01020304 ; with alpha\n"
        "colour.deck = dark-red\n"
        "colour.rib = cyan\n"
        "role.exec = #102030 #405060\n"
        "role.vram = yellow\n"
        "font.ui = Some Font.ttf\n"
        "metric.strip.height = 42.5\n"
        "panel.light = yes\n",
        skin);
    check(errors.empty(), "all value forms parse");
    check(skin.panel_light, "yes/no values");
    check(skin.frame == Colour{1, 2, 3, 4}, "#rrggbbaa");
    check(skin.deck == Colour{146, 0, 0}, "dark- gives the I = 0 level");
    check(skin.rib == Colour{0, 255, 255}, "a TVC name gives the bright variant");
    check(skin.role(Role::Exec).on_case == Colour{0x10, 0x20, 0x30} && skin.role(Role::Exec).on_putty == Colour{0x40, 0x50, 0x60},
          "a role with two colours");
    check(skin.role(Role::Vram).on_case == Colour{255, 255, 0} && skin.role(Role::Vram).on_putty == Colour{146, 146, 0},
          "a role with a TVC name");
    check(skin.font_ui == "Some Font.ttf", "string values keep inner spaces");
    check(skin.strip_height == 42.5f, "numbers");

    Skin untouched;
    errors = freetvc::sdl::parse_skin(
        "\n; comment\ncolour.nope = #000000\ncolour.key = #12345\nmetric.groove = -1\nno equals\nrole.exec = mauve\npanel.light = maybe\n",
        untouched);
    check(errors.size() == 6, "six bad lines reported");
    check(!errors.empty() && errors[0].starts_with("line 3:"), "line numbers count comments and blank lines");
    check(untouched == Skin{}, "bad lines change nothing");

    const auto list = freetvc::sdl::list_skins({dir, dir / "missing"});
    check(list.size() >= 2 && list[0].name == "bronze" && list[1].name == "deck", "list_skins sorted by name");
    check(freetvc::sdl::find_skin("deck", {dir}) == dir / "deck.skin", "find_skin by name");
    check(freetvc::sdl::find_skin((dir / "deck.skin").string(), {}).has_value(), "find_skin by path");
    check(!freetvc::sdl::find_skin("missing", {dir}).has_value(), "find_skin misses");

    if (g_failures == 0) {
        std::printf("skin_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
