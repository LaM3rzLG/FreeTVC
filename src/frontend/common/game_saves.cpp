// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/game_saves.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <system_error>

#include "core/message.hpp"

namespace fs = std::filesystem;

namespace freetvc {

namespace {

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

}  // namespace

fs::path default_save_root(const fs::path& user_dir) { return user_dir / "SaveState"; }

std::string game_folder_name(std::string_view name) {
    std::string clean;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        clean += u < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ? '_' : c;
    }
    // Windows drops trailing dots and spaces, and leading ones hide or confuse a folder elsewhere
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) {
        clean.pop_back();
    }
    clean.erase(0, std::min(clean.find_first_not_of(". "), clean.size()));
    if (clean.empty()) {
        return "TVC";
    }
    std::string upper = clean.substr(0, clean.find('.'));
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    static constexpr std::array<std::string_view, 4> kReserved{"CON", "PRN", "AUX", "NUL"};
    const bool numbered = upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' && upper[3] <= '9';
    if (numbered || std::find(kReserved.begin(), kReserved.end(), upper) != kReserved.end()) {
        clean += '_';
    }
    return clean;
}

std::vector<std::string> list_games(const fs::path& root) {
    std::vector<std::string> games;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            games.push_back(utf8(it->path().filename()));
        }
    }
    std::sort(games.begin(), games.end());
    return games;
}

std::vector<GameSave> list_saves(const fs::path& root, std::string_view game) {
    struct Found {
        GameSave save;
        fs::file_time_type time;
    };
    std::vector<Found> found;
    std::error_code ec;
    for (fs::directory_iterator it(root / from_utf8(game_folder_name(game)), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& file = it->path();
        if (!it->is_regular_file(ec) || file.extension() != ".tvcs") {
            continue;
        }
        fs::path picture = fs::path(file).replace_extension(".png");
        std::error_code pec;
        const fs::file_time_type time = fs::last_write_time(file, pec);
        // file_clock to system_clock through the two clocks' present
        const auto system = std::chrono::system_clock::now() +
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now());
        const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &seconds);
#else
        localtime_r(&seconds, &local);
#endif
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
        found.push_back({{file, fs::is_regular_file(picture, pec) ? picture : fs::path(), utf8(file.stem()), when}, time});
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.time > b.time; });
    std::vector<GameSave> saves;
    for (Found& f : found) {
        saves.push_back(std::move(f.save));
    }
    return saves;
}

fs::path new_save_file(const fs::path& root, std::string_view game, std::string& error) {
    const std::string name = game_folder_name(game);
    const fs::path dir = root / from_utf8(name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = msg("cannot make %s: %s", utf8(dir).c_str(), ec.message().c_str());
        return {};
    }
    // One past the highest "<name> NNN" there, so a deleted save never gives its number to a newer one
    unsigned highest = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string stem = utf8(it->path().stem());
        if (it->path().extension() == ".tvcs" && stem.size() > name.size() + 1 && stem.starts_with(name + " ")) {
            const std::string digits = stem.substr(name.size() + 1);
            if (digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 9) {
                highest = std::max(highest, static_cast<unsigned>(std::stoul(digits)));
            }
        }
    }
    char number[16];
    std::snprintf(number, sizeof number, " %03u.tvcs", highest + 1);
    return dir / from_utf8(name + number);
}

fs::path named_save_file(const fs::path& root, std::string_view game, std::string_view name) {
    return root / from_utf8(game_folder_name(game)) / from_utf8(game_folder_name(name) + ".tvcs");
}

bool delete_save(const GameSave& save, std::string& error) {
    std::error_code ec;
    if (!fs::remove(save.file, ec)) {
        error = ec ? msg("cannot delete %s: %s", utf8(save.file).c_str(), ec.message().c_str()) : msg("no such save: %s", utf8(save.file).c_str());
        return false;
    }
    if (!save.picture.empty()) {
        fs::remove(save.picture, ec);
    }
    return true;
}

}  // namespace freetvc
