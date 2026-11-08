// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the screen in its frame and the status strip with the menu (design/ui.md "Play").
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/video/framebuffer.hpp"
#include "frontend/sdl/deck.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/sdl/host_input.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "frontend/sdl/translation.hpp"
#include "imgui.h"

namespace freetvc::sdl {

// Screen areas live in the core (core/video/framebuffer.hpp), shared with the monitor's screenshots
using freetvc::kFullRaster;
using freetvc::kTvArea;
using freetvc::ScreenArea;

struct GameSaveItem {
    std::string name;  // the file name without the extension
    std::string when;  // the file's time
    std::string file;
};
struct GameSaveList {
    std::string game;
    std::vector<GameSaveItem> saves;  // newest first
};

struct PlayStatus {
    ImTextureID screen = ImTextureID_Invalid;  // the framebuffer texture, 800 x 314
    // At start: a faint photo of the machine over a dark screen until the machine is ready; 0 = gone
    ImTextureID splash = ImTextureID_Invalid;
    ImVec2 splash_size{};
    float splash_alpha = 0.0f;
    std::span<const Preset> presets;
    std::size_t preset = 0;
    bool paused = false;
    double speed_percent = -1.0;  // negative until the first measurement
    KeyMapping mapping = KeyMapping::Symbolic;
    std::span<const KeymapEntry> layouts;  // host key layouts
    std::span<const std::string> recent;   // the last programs loaded, newest first
    std::string_view game;                    // the program the game saves go to
    std::span<const GameSaveList> game_saves;  // every game with saves, `game` first when it has any
    std::string_view layout;
    bool audio = false;  // an audio device is open
    bool muted = false;
    bool blending = false;  // frame blending on the window's display now
    int blend_sharpness = 50;  // percent
    bool fullscreen = false;
    bool full_raster = false;  // show kFullRaster with the TV area outlined, else kTvArea
    std::span<const SkinEntry> skins;
    std::string_view skin_name;
    std::span<const LanguageFile> languages;  // English is always there
    std::string_view language;
    std::string_view notice;
    bool notice_is_error = false;
    double notice_age = 0.0;  // seconds since the notice was set
    float scale = 1.0f;       // display content scale
    std::string_view cassette;  // TVC name of the attached CAS file; empty = none
    bool can_browse = true;     // a file dialog can be shown
    bool bench = false;         // the window shows Bench
    // The tape deck
    bool tape_loaded = false;
    std::string_view tape_state;  // "play", "rec" or "stop"
    std::string_view tape_counter;  // "1:23"
    bool tape_moving = false;       // PLAY or RECORD down and the motor on
    std::span<const float> tape_wave;  // the signal before the head while it moves (DeckRow::tape_wave)
    bool tape_recording = false;
    bool fast_tape = true;
    std::string_view cartridge;  // file name of the cartridge; empty = none
    bool quick_snapshot = false;  // the program running has a game save
    bool game_saved = false;      // a game save was just made: its key is lit
    // The disk card and drives A-D: file names, empty = no disk
    bool disk_card = false;
    std::array<std::string_view, 4> disks{};
    int disk_busy = -1;  // the drive the controller is working with, -1 = none
    bool fast_disk = true;
    int gamecard_slot = -1;  // the GameCard's slot, -1 = out
    int multichip_slot = -1;  // the Multichip sound card's slot, -1 = out
    int sid_card = 0;         // the SID card at E0h-FFh: 0 out, 1 MOS 6581, 2 MOS 8580
    int card256_slot = -1;   // the TVC256k++'s slot, -1 = out
    int twodfx_slot = -1;    // the 2dfx graphics card's slot, -1 = out
    std::array<CardKind, kCardSlots> slot_kinds{};
    std::string_view disk_rom;     // the disk card's EPROM: "1.1", "1.2" or a file  // the card in each slot (None = empty)
    bool recording = false;        // a video is being recorded
    double record_seconds = 0.0;   // its machine time so far
    int card256_usb = 0;     // its USB port: 0 empty, 1 pendrive, 2 mouse
    std::string_view card256_pendrive;  // the pendrive folder's name
    std::string_view card256_flash;     // the flash drive's image or folder name; empty = none
    int nanosd_slot = -1;           // the NanoSD's slot, -1 = out
    std::string_view nanosd_sd;     // its SD card folder's name; empty = an empty socket
    bool sddual = false;            // the SD Dual in the cartridge port
    std::array<std::string_view, 2> sd_cards{};  // its sockets' images (microSD, SD); empty = an empty socket
    bool mouse_captured = false;
    bool printer = false;              // a printer on the Centronics port; none by default
    std::string_view printer_file;     // its output file's name; empty = kept in memory only
    bool printing = false;             // bytes reached the printer in the last moment
    int serial_slot = -1;              // the serial card's slot, -1 = out
    std::string_view serial_line;      // its cable: "off", "on", "listen:PORT", "connect:PORT"
    bool serial_connected = false;     // a peer is there (/CTS and /DSR on)
    std::string_view serial_capture;   // the capture file's name; empty = none
    std::array<std::string_view, 4> slot_cards{};  // what each expansion slot holds, empty = nothing (the slot ribs)
    float side_panel = 0.0f;  // width the settings sheet takes at the right
    float bottom_panel = 0.0f;  // height the keyboard editor takes above the strip
    PictureHook picture;      // the tube in Play; empty = the plain image
    bool settings = false;    // the settings sheet is open
};

struct PlayActions {
    bool menu_open = false;
    float strip_width = 0.0f;  // the width the strip's keys need side by side (window points)
    float strip_drag = 0.0f;   // the strip's top edge dragged by this many points (up is negative)
    std::string layout;        // a host key layout file to use
    std::string open_recent;   // a program from the recent list to load
    std::string load_save;     // a game save to load
    bool edit_layout = false;  // the keyboard editor opens  // keys must not reach the TVC while it is
    bool toggle_pause = false;
    bool reset = false;
    bool power_cycle = false;
    bool screenshot = false;
    bool toggle_fullscreen = false;
    bool toggle_full_raster = false;
    bool toggle_mute = false;
    bool toggle_blend = false;  // frame blending on or off
    int blend_sharpness = -1;            // a new sharpness, shown at once
    bool blend_sharpness_done = false;   // the slider let go: written to freetvc.ini
    bool reload_skin = false;
    bool quit = false;
    bool load_program = false;    // file dialog, then autostart
    bool disk_autostart = false;  // file dialog for a disk image, then its first program starts from A:
    bool run_again = false;     // autostart the attached cassette
    bool eject = false;
    bool toggle_bench = false;
    bool toggle_settings = false;  // the settings sheet
    bool open_about = false;
    bool tape_insert = false;     // file dialog, then into the deck
    bool tape_insert_play = false;  // the same, then PLAY goes down (the strip's play key with no tape)
    bool tape_run = false;        // autostart from the tape in the deck
    bool tape_play = false;
    bool tape_stop = false;
    bool tape_rewind = false;
    bool tape_eject = false;
    bool tape_record = false;     // save dialog, then a blank tape recording into that WAV
    bool toggle_fast_tape = false;
    bool cart_insert = false;  // file dialog, then the cartridge goes in and the machine powers on
    bool cart_remove = false;
    bool multicart_new = false;  // file dialog for its programs, a save dialog for the image, then it goes in
    bool snapshot_save = false;  // save dialog, then a TVCS file
    bool snapshot_load = false;  // file dialog, then the machine becomes the snapshot
    bool quick_save = false;
    bool quick_load = false;
    bool open_save_dir = false;
    bool toggle_disk_card = false;  // the card goes in or out and the machine powers on
    bool toggle_fast_disk = false;
    bool toggle_gamecard = false;  // the GameCard goes in (slot 1) or out and the machine powers on
    bool toggle_multichip = false;  // the Multichip sound card goes in (slot 2) or out and the machine powers on
    int sid_card = -1;  // the SID card becomes 0 out, 1 a MOS 6581, 2 a MOS 8580, and the machine powers on; -1 = no change
    bool toggle_card256 = false;   // the TVC256k++ goes in (slot 0 or the first free) or out and the machine powers on
    bool card256_pendrive = false;  // folder dialog, then that folder is the pendrive on its USB port
    bool card256_mouse = false;     // the mouse on its USB port
    bool card256_unplug = false;    // nothing on its USB port
    bool card256_flash = false;     // file dialog, then that image is its flash drive
    std::string disk_rom;              // "1.1" or "1.2": the disk card's new EPROM (powers on while the card is in)
    int slot_clear = -1;               // the card in this slot comes out and the machine powers on
    bool toggle_record = false;        // the REC key: a video starts (named after the program and the time) or stops
    bool video_dir = false;            // folder dialog, then recordings go there
    bool open_video_dir = false;
    bool screenshot_dir = false;       // folder dialog, then screenshots go there
    bool open_screenshot_dir = false;
    bool toggle_twodfx = false;     // the 2dfx card goes in (slot 2, else the first free) or out and the machine powers on
    bool toggle_nanosd = false;     // the NanoSD goes in (slot 3) or out and the machine powers on
    bool nanosd_sd = false;         // folder dialog, then that folder is its SD card
    bool nanosd_eject = false;      // its SD card comes out
    bool toggle_sddual = false;     // the SD Dual goes in or out of the cartridge port and the machine powers on
    int sd_insert = -1;             // socket: file dialog, then that image is its card
    int sd_eject = -1;              // socket: its card comes out
    bool sd_new = false;            // file dialog for the files, a save dialog for the image, then it goes into the microSD socket
    bool printer_file = false;       // save dialog, then a printer printing to that file (.txt: UTF-8 text, else raw)
    bool printer_unplug = false;
    bool toggle_serial = false;      // the serial card goes in (slot 1, else the first free) or out and the machine powers on
    int serial_line = -1;            // its cable: 0 unplugged, 1 listening on 127.0.0.1:6585
    CardKind slot_card = CardKind::None;  // this card goes into `slot_card_to` (put in or moved) and the machine powers on
    int slot_card_to = -1;
    bool serial_send_file = false;   // file dialog, then its bytes go to the TVC
    bool serial_capture = false;     // save dialog, then what the TVC sends is appended there
    bool serial_capture_stop = false;
    bool fetch_roms = false;  // download the ROMs into the user data folder
    int disk_insert = -1;           // drive: file dialog, then the disk goes in
    int disk_eject = -1;
    int preset = -1;  // index into PlayStatus::presets
    int mapping = -1;  // KeyMapping value
    std::string skin;  // path of the skin to switch to
    std::string language;  // code of the UI language to switch to
};

PlayActions draw_play(const PlayStatus& status, const Skin& skin, const SkinFonts& fonts);

// The status strip along the bottom of the window, with its menus; Play and Bench both show it.
// `notice`: the latest notice goes into the strip (Bench); Play shows it on the picture's top border instead
void draw_strip(const PlayStatus& status, const Skin& skin, const SkinFonts& fonts, PlayActions& actions, bool notice = true);
// Height the strip and its groove take at the bottom of the window.
float strip_space(const Skin& skin, float scale);

}  // namespace freetvc::sdl
