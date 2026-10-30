// SPDX-License-Identifier: GPL-3.0-or-later
// freetvc.ini: parsing, the portable place, and rewriting keys in place.
// usage: settings_test <scratch dir>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "frontend/common/settings.hpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n%s\n", what, detail.c_str());
    }
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: settings_test <scratch dir>\n");
        return 2;
    }
    const fs::path scratch = fs::path(argv[1]) / "settings";
    fs::remove_all(scratch);
    const fs::path program = scratch / "program";
    const fs::path user = scratch / "user";
    fs::create_directories(program);

    // Parsing: values, comments, a later line winning, relative rom_dir against the file's folder, errors
    std::vector<std::string> errors;
    const freetvc::Settings s = freetvc::parse_settings(
        "; comment\n"
        "preset = 64k-1.2  ; the old one\n"
        "skin = deck\n"
        "keymap = positional\n"
        "audio = no\n"
        "full_raster = yes\n"
        "rom_dir = roms\n"
        "preset = 64k+-2.2\n"
        "keymap = sideways\n"
        "colour = red\n"
        "no equals sign\n"
        "disk_card = yes\n"
        "gamecard = no\n"
        "multichip = yes\n"
        "sid_card = 8580\n"
        "card256 = yes\n"
        "nanosd = yes\n"
        "nanosd_sd = sd\n"
        "nanosd_menu = no\n"
        "nanosd_sort = size\n",
        program, errors);
    check(s.preset == "64k+-2.2" && s.skin == "deck" && s.keymap == "positional" && s.audio == false && s.full_raster == true &&
              s.disk_card == true && s.gamecard == false && s.multichip == true && s.card256 == true && s.nanosd == true &&
              s.nanosd_menu == false && s.nanosd_sort == 2 && s.sid_card == "8580",
          "values, comments and the last line winning");
    check(s.nanosd_sd == (program / "sd").lexically_normal().string(), "a relative nanosd_sd is read against the file's folder",
          s.nanosd_sd.value_or(""));
    std::vector<std::string> sort_errors;
    check(!freetvc::parse_settings("nanosd_sort = 7\nnanosd_sd = none\n", program, sort_errors).nanosd_sort && sort_errors.size() == 1 &&
              freetvc::parse_settings("nanosd_sd = none\n", program, sort_errors).nanosd_sd == "none",
          "nanosd_sort takes none, name or size; nanosd_sd none stays none");
    // The Settings window's keys
    std::vector<std::string> d53_errors;
    const freetvc::Settings d53 = freetvc::parse_settings(
        "disk_rom = 1.2\nvolume = 70\nfullscreen = yes\nmonitor = on\nmcp = 7000\ndzrp = off\ngdb = 6583\ndzrp_banks = yes\n"
        "mcp_allow_monitor = no\nvolume = 101\ngdb = 70000\nmcp = maybe\n",
        program, d53_errors);
    check(d53.disk_rom == "1.2" && d53.volume == 70 && d53.fullscreen == true && d53.monitor == freetvc::kServerDefaultPort &&
              d53.mcp == 7000 && d53.dzrp == freetvc::kServerOff && d53.gdb == 6583 && d53.dzrp_banks == true &&
              d53.mcp_allow_monitor == false && d53_errors.size() == 3,
          "the Settings window's keys, bad volume and ports refused");
    std::vector<std::string> rom_errors;
    check(freetvc::parse_settings("disk_rom = my.rom\n", program, rom_errors).disk_rom == (program / "my.rom").lexically_normal().string(),
          "a disk_rom file is read against the file's folder");
    std::vector<std::string> sid_errors;
    check(!freetvc::parse_settings("sid_card = 6582\n", program, sid_errors).sid_card && sid_errors.size() == 1 &&
              freetvc::parse_settings("sid_card = off\n", program, sid_errors).sid_card == "off",
          "sid_card takes off, 6581 or 8580");
    std::vector<std::string> d48_errors;
    const freetvc::Settings d48 = freetvc::parse_settings(
        "printer = out.prn\nserial_card = yes\nserial_line = listen:7000\nserial_flow = none\nserial_line = cable\nserial_flow = xon\n", program, d48_errors);
    check(d48.printer == (program / "out.prn").lexically_normal().string() && d48.serial_card == true && d48.serial_line == "listen:7000" &&
              d48.serial_flow == "none" && d48_errors.size() == 2,
          "printer (a relative file against the file's folder), serial_card, serial_line and serial_flow; bad values reported");
    check(freetvc::parse_settings("printer = off\n", program, d48_errors).printer == "off", "printer off stays off");
    check(s.rom_dir == (program / "roms").lexically_normal().string(), "a relative rom_dir is read against the file's folder",
          s.rom_dir.value_or(""));
    check(errors.size() == 3 && errors[0].starts_with("freetvc.ini line 9:"), "bad lines reported with their numbers",
          errors.empty() ? std::string() : errors[0]);

    // Place: the user data folder, unless a freetvc.ini sits next to the program
    bool portable = true;
    check(freetvc::settings_path(program, user, &portable) == user / "freetvc.ini" && !portable, "user data folder by default");
    std::ofstream(program / "freetvc.ini") << "skin = deck\n";
    check(freetvc::settings_path(program, user, &portable) == program / "freetvc.ini" && portable, "portable when present");
    const freetvc::SettingsFile loaded = freetvc::load_settings(program, user);
    check(loaded.portable && loaded.values.skin == "deck", "load_settings reads the portable file");
    fs::remove(program / "freetvc.ini");
    check(!freetvc::load_settings(program, user).values.skin, "a missing file gives no values");

    // Rewriting: lines changed in place, comments and unknown lines kept, missing keys appended
    const std::string before =
        "; my settings\n"
        "rom_dir = /opt/tvc  ; shared\n"
        "preset = 64k-1.2  ; the old one\n"
        "something = else\n";
    const std::string after = freetvc::update_settings_text(before, {{"preset", "64k+-2.2"}, {"keymap", "positional"}});
    check(after ==
              "; my settings\n"
              "rom_dir = /opt/tvc  ; shared\n"
              "preset = 64k+-2.2  ; the old one\n"
              "something = else\n"
              "keymap = positional\n",
          "update in place", after);
    const std::string removed = freetvc::update_settings_text(before, {{"rom_dir", ""}, {"volume", ""}});
    check(removed == "; my settings\npreset = 64k-1.2  ; the old one\nsomething = else\n", "an empty value removes the key", removed);

    // Saving creates the folder and a commented file, and a second save keeps the first one's keys
    std::string error;
    const fs::path file = user / "freetvc.ini";
    check(freetvc::save_settings(file, {{"preset", "64k-1.2"}, {"skin", "bronze"}}, error), "first save", error);
    check(freetvc::save_settings(file, {{"skin", "deck"}}, error), "second save", error);
    const std::string text = read(file);
    check(text.starts_with("; freeTVC settings") && text.find("preset = 64k-1.2\n") != std::string::npos &&
              text.find("skin = deck\n") != std::string::npos && text.find("skin = bronze") == std::string::npos,
          "saved file", text);
    errors.clear();
    check(freetvc::parse_settings(text, user, errors).skin == "deck" && errors.empty(), "the saved file reads back", text);
    check(!fs::exists(user / "freetvc.ini.new"), "no temporary file left");

    fs::remove_all(scratch);
    if (g_failures == 0) {
        std::printf("settings_test: all checks passed\n");
        return 0;
    }
    std::printf("settings_test: %d check(s) failed\n", g_failures);
    return 1;
}
