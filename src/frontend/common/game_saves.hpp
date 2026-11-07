// SPDX-License-Identifier: GPL-3.0-or-later
// Game saves (design/media.md "Game saves"): snapshots kept per program in <root>/<game>/<date time>.tvcs with a picture.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

struct GameSave {
    std::filesystem::path file;     // the .tvcs
    std::filesystem::path picture;  // the .png beside it; empty when there is none
    std::string name;               // the file name without the extension: its date and time
};

// The default root: SaveState in the user data folder.
std::filesystem::path default_save_root(const std::filesystem::path& user_dir);
// `name` as a folder name that works on Linux, Windows and macOS; "TVC" when nothing is left.
std::string game_folder_name(std::string_view name);
// The games that have a folder under `root`, sorted.
std::vector<std::string> list_games(const std::filesystem::path& root);
// The saves of `game`, newest first.
std::vector<GameSave> list_saves(const std::filesystem::path& root, std::string_view game);
// A new file for `game` named after `when` (local time), "YYYY-MM-DD HH-MM-SS.tvcs", with " (2)" and on when taken; the
// folder is made. Empty with `error` set when it cannot be.
std::filesystem::path new_save_file(const std::filesystem::path& root, std::string_view game, std::time_t when, std::string& error);
// Deletes a save and its picture.
bool delete_save(const GameSave& save, std::string& error);

}  // namespace freetvc
