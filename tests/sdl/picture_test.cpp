// SPDX-License-Identifier: GPL-3.0-or-later
// Picture files (design/ui.md "Picture"): the shipped ones parse, pal-tv.picture names every key, values out of
// range and unknown keys are reported with their line, and picture_text() reads back to the same values.
// usage: picture_test <data/pictures directory>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "frontend/sdl/picture.hpp"

namespace {

using freetvc::sdl::Picture;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: picture_test <data/pictures directory>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const auto entries = freetvc::sdl::list_pictures({dir});
    check(entries.size() == 4, "four shipped pictures");
    for (const auto& entry : entries) {
        std::string error;
        const auto picture = freetvc::sdl::load_picture_file(entry.path, error);
        check(picture.has_value(), entry.name + " loads: " + error);
        check(picture && picture->name == entry.name, entry.name + " is named after its file");
    }

    std::string error;
    const auto pal = freetvc::sdl::load_picture_file(dir / "pal-tv.picture", error);
    check(pal && pal->signal == freetvc::sdl::Signal::Pal && pal->mask == freetvc::sdl::Mask::Shadow && pal->delay_line, "pal-tv's signal and mask");
    std::ifstream file(dir / "pal-tv.picture");
    std::ostringstream text;
    text << file.rdbuf();
    std::istringstream keys(freetvc::sdl::picture_text(Picture{}));
    for (std::string line; std::getline(keys, line);) {
        const std::string key = line.substr(0, line.find(" = "));
        check(text.str().find("\n" + key + " ") != std::string::npos, "pal-tv.picture lists " + key);
    }

    Picture round;
    round.signal = freetvc::sdl::Signal::Mono;
    round.mask = freetvc::sdl::Mask::Grille;
    round.delay_line = false;
    round.glow = 0.25f;
    round.curvature = 0.125f;
    Picture back;
    check(freetvc::sdl::parse_picture(freetvc::sdl::picture_text(round), back).empty() && back == round, "picture_text reads back");

    Picture bad;
    const auto errors = freetvc::sdl::parse_picture("signal = pal\n; fine\nglow = 3\nfocus = 1\nmask = dots\nbeam.min = 0.5\nbeam.max = 0.4\n", bad);
    check(errors.size() == 4 && errors[0].starts_with("line 3:") && errors[1].starts_with("line 4:") && errors[2].starts_with("line 5:"),
          "bad lines reported with their number");
    check(bad.signal == freetvc::sdl::Signal::Pal && bad.glow == Picture{}.glow, "good lines kept, bad values ignored");

    std::printf("%s\n", g_failures ? "picture_test: failures" : "picture_test: ok");
    return g_failures ? 1 : 0;
}
