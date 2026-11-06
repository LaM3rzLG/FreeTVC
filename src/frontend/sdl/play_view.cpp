// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/play_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "frontend/sdl/deck.hpp"
#include "frontend/sdl/text.hpp"

namespace freetvc::sdl {

namespace {

constexpr double kNoticeSeconds = 6.0;
constexpr double kErrorSeconds = 15.0;

// design/ui.md "Strip menus": one popup per strip item
constexpr const char* kMainMenu = "##main_menu";
constexpr const char* kMachineMenu = "##machine_menu";
constexpr const char* kCardsMenu = "##cards_menu";
constexpr const char* kTapeMenu = "##tape_menu";
constexpr const char* kDiskMenu = "##disk_menu";
constexpr const char* kCartMenu = "##cart_menu";
constexpr const char* kKeysMenu = "##keys_menu";
constexpr const char* kPrinterMenu = "##printer_menu";
constexpr const char* kRecordMenu = "##record_menu";
constexpr const char* kShotMenu = "##shot_menu";

// Opens `id` above the strip at window x `anchor`; true while it is open (EndPopup is the caller's)
bool begin_menu(const char* id, float anchor, float strip_top, PlayActions& actions, float pivot_x = 0.0f) {
    ImGui::SetNextWindowPos({anchor, strip_top}, ImGuiCond_Appearing, {pivot_x, 1.0f});
    if (!ImGui::BeginPopup(id)) {
        return false;
    }
    actions.menu_open = true;
    // Keyboard navigation is off, so ImGui itself would not close it on Escape
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    return true;
}

// True when the last strip item should open menu `id`: clicked, or clicked while another strip menu is open (switches menus in one click)
bool opens(bool clicked, const char* id) {
    return clicked || (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsPopupOpen(id) &&
                       ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
}

void draw_main_menu(const PlayStatus& status, PlayActions& actions) {
    // design/media.md: a dropped .cas file autostarts too (Ctrl: attach only)
    actions.load_program |= ImGui::MenuItem(tr("Load program..."), nullptr, false, status.can_browse);
    if (ImGui::BeginMenu(tr("Recent programs"), !status.recent.empty())) {
        for (const std::string& path : status.recent) {
            const std::size_t slash = path.find_last_of("/\\");
            if (ImGui::MenuItem(slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1)) {
                actions.open_recent = path;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", path.c_str());
            }
        }
        ImGui::EndMenu();
    }
    const std::string cassette(status.cassette);
    char item[160];
    std::snprintf(item, sizeof item, tr("Run %s again"), cassette.c_str());
    actions.run_again |= ImGui::MenuItem(cassette.empty() ? tr("Run again") : item, nullptr, false, !cassette.empty());

    ImGui::Separator();
    if (ImGui::BeginMenu(tr("Snapshot"))) {
        actions.quick_save |= ImGui::MenuItem(tr("Quick save"), "F9");
        actions.quick_load |= ImGui::MenuItem(tr("Quick load"), "Shift+F9", false, status.quick_snapshot);
        actions.snapshot_save |= ImGui::MenuItem(tr("Save snapshot..."), nullptr, false, status.can_browse);
        actions.snapshot_load |= ImGui::MenuItem(tr("Load snapshot..."), nullptr, false, status.can_browse);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    actions.toggle_fullscreen |= ImGui::MenuItem(tr("Full screen"), "F11", status.fullscreen);
    if (ImGui::BeginMenu(tr("Skin"))) {
        for (const SkinEntry& entry : status.skins) {
            if (ImGui::MenuItem(entry.name.c_str(), nullptr, entry.name == status.skin_name)) {
                actions.skin = entry.path.string();
            }
        }
        ImGui::Separator();
        actions.reload_skin |= ImGui::MenuItem(tr("Reload skin"));
        ImGui::EndMenu();
    }
    // Each language under its own name
    if (ImGui::BeginMenu(tr("Language"))) {
        if (ImGui::MenuItem("English", nullptr, status.language == "en")) {
            actions.language = "en";
        }
        for (const LanguageFile& file : status.languages) {
            if (ImGui::MenuItem(file.name.c_str(), nullptr, status.language == file.code)) {
                actions.language = file.code;
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    actions.fetch_roms |= ImGui::MenuItem(tr("Download ROMs"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("The TVC's ROMs from tvc.homeserver.hu and the cards' firmware from their authors' sites, with the system's curl, into the ROM folder chosen in Settings (rom_dir), else the user data folder"));
    }
    actions.toggle_settings |= ImGui::MenuItem(tr("Settings..."), nullptr, status.settings);
    actions.toggle_bench |= ImGui::MenuItem(tr("Bench"), "F12", status.bench);
    actions.open_about |= ImGui::MenuItem(tr("About freeTVC..."));
    actions.quit |= ImGui::MenuItem(tr("Quit"));
}

void draw_ports_menu(const PlayStatus& status, PlayActions& actions);

// The preset with this board, or -1
int preset_of(const PlayStatus& status, Model model) {
    for (std::size_t i = 0; i < status.presets.size(); ++i) {
        if (status.presets[i].model == model) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void draw_machine_menu(const PlayStatus& status, PlayActions& actions) {
    // The factory machines: the board, and its BASIC beside it; the HBM RAM card is in the Cards menu
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextAlign, {0.5f, 0.5f});
    ImGui::SeparatorText(tr("Version"));
    ImGui::PopStyleVar();
    const Model current = status.presets[status.preset].model;
    for (std::size_t i = 0; i < status.presets.size(); ++i) {
        const Preset& p = status.presets[i];
        if (p.model == Model::Tvc32kHbm) {
            continue;
        }
        const char* board = p.model == Model::Tvc32k ? "TVC 32K" : p.model == Model::Tvc64k ? "TVC 64K" : "TVC 64K+";
        const std::string basic = "BASIC " + std::string(p.name.substr(p.name.rfind('-') + 1));
        const bool on = i == status.preset || (p.model == Model::Tvc32k && current == Model::Tvc32kHbm);
        ImGui::PushID(static_cast<int>(i));  // two rows read "TVC 64K+"
        if (ImGui::MenuItem(board, basic.c_str(), on) && !on) {
            actions.preset = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", p.model == Model::Tvc64kPlus ? tr("SYS and EXT 2.x ROMs") : tr("SYS 1.2 and EXT 1.x ROMs"));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    actions.toggle_pause |= ImGui::MenuItem(tr("Pause"), "Pause", status.paused);
    actions.reset |= ImGui::MenuItem(tr("Reset"));
    actions.power_cycle |= ImGui::MenuItem(tr("Power cycle"));
}

void draw_disk_menu(const PlayStatus& status, PlayActions& actions);

// A card's own media and cable, under its slot; none of this powers the machine on
void draw_card_extras(CardKind kind, const PlayStatus& status, PlayActions& actions) {
    char item[160];
    switch (kind) {
    case CardKind::Hbf: {
        draw_disk_menu(status, actions);
        // The card's EPROM: the disk system it boots
        ImGui::SeparatorText(tr("Disk system"));
        for (const auto& [choice, label] : {std::pair{"1.1", "VT-DOS 1.1"}, std::pair{"1.2", "VT-DOS 1.2"}}) {
            if (ImGui::MenuItem(label, nullptr, status.disk_rom == choice) && status.disk_rom != choice) {
                actions.disk_rom = choice;
            }
        }
        ImGui::MenuItem(tr("UPM (CP/M)"), nullptr, false, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Videoton's CP/M: its card EPROM and cartridge are not in the ROM download yet"));
        }
        break;
    }
    case CardKind::Card256: {
        // One USB port, so the pendrive and the mouse take turns; none of this powers the machine on
        const std::string pendrive(status.card256_pendrive);
        std::snprintf(item, sizeof item, tr("Pendrive %s..."), pendrive.c_str());
        actions.card256_pendrive |= ImGui::MenuItem(status.card256_usb == 1 ? item : tr("Pendrive folder..."), nullptr, status.card256_usb == 1,
                                                    status.can_browse);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A host folder on the card's USB port as drive 0: LOAD and SAVE go there, files are written at once"));
        }
        actions.card256_mouse |= ImGui::MenuItem(tr("Mouse"), nullptr, status.card256_usb == 2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("The host mouse on the card's USB port: click the screen to capture it, Ctrl+Alt lets it go"));
        }
        actions.card256_unplug |= ImGui::MenuItem(tr("USB port empty"), nullptr, status.card256_usb == 0);
        const std::string flash(status.card256_flash);
        std::snprintf(item, sizeof item, tr("Flash drive %s..."), flash.c_str());
        actions.card256_flash |= ImGui::MenuItem(flash.empty() ? tr("Flash drive image...") : item, nullptr, false, status.can_browse);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A littlefs image as drive 2, read only: the card's launcher and games (card256-flash.bin)"));
        }
        break;
    }
    case CardKind::NanoSd: {
        // The SD card comes and goes like on the card, without powering on; the Nano notices
        const std::string sd(status.nanosd_sd);
        std::snprintf(item, sizeof item, tr("SD card %s..."), sd.c_str());
        actions.nanosd_sd |= ImGui::MenuItem(sd.empty() ? tr("SD card folder...") : item, nullptr, !sd.empty(), status.can_browse);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A host folder as the SD card: the card sees 8.3 names only, files are written at once"));
        }
        actions.nanosd_eject |= ImGui::MenuItem(tr("Take the SD card out"), nullptr, false, !sd.empty());
        break;
    }
    case CardKind::Serial: {
        // The cable and its files change without powering on
        actions.serial_line = ImGui::MenuItem(tr("Cable unplugged"), nullptr, status.serial_line == "off") ? 0 : actions.serial_line;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("No peer: sending waits for DSR until STOP (CTRL+ESC)"));
        }
        const bool listening = status.serial_line.starts_with("listen");
        if (listening) {
            const std::string line(status.serial_line.substr(7));
            std::snprintf(item, sizeof item, status.serial_connected ? tr("TCP port %s, connected") : tr("TCP port %s, waiting"), line.c_str());
        }
        actions.serial_line = ImGui::MenuItem(listening ? item : tr("Cable to TCP port 6585"), nullptr, listening) ? 1 : actions.serial_line;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A TCP client on 127.0.0.1 is the other end of the cable, such as another freeTVC started with "
                                       "--serial connect:6585"));
        }
        actions.serial_send_file |= ImGui::MenuItem(tr("Send a file..."), nullptr, false, status.can_browse && status.serial_line != "off");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Its bytes arrive at the TVC while it reads (LOAD #6, INPUT #6)"));
        }
        const std::string capture(status.serial_capture);
        std::snprintf(item, sizeof item, tr("Capturing to %s"), capture.c_str());
        if (capture.empty()) {
            actions.serial_capture |= ImGui::MenuItem(tr("Capture to a file..."), nullptr, false, status.can_browse);
        } else {
            actions.serial_capture_stop |= ImGui::MenuItem(item, nullptr, true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("What the TVC sends is appended to the file (with no other peer the cable then goes to the host)"));
        }
        break;
    }
    default:
        break;
    }
}

bool has_card_extras(CardKind kind) { return kind == CardKind::Hbf || kind == CardKind::Card256 || kind == CardKind::NanoSd || kind == CardKind::Serial; }

void draw_cards_menu(const PlayStatus& status, PlayActions& actions) {
    // design/cards.md: a slot's list puts a card in (moving it from another slot, taking out what was there); the machine powers on
    ImGui::SeparatorText(tr("Cards"));
    struct Choice {
        CardKind kind;
        const char* name;
        const char* tip;
    };
    const Choice choices[] = {
        {CardKind::None, tr("Empty"), nullptr},
        {CardKind::Hbf, tr("HBF disk card"), tr("HBF card in slot 0: LOAD and SAVE use the disks, not the cassette")},
        {CardKind::GameCard, tr("GameCard"), tr("dikdom's card: SN76489 sound, joysticks 3 and 4 on gamepads 3 and 4, a boot menu with four games")},
        {CardKind::Multichip, tr("Multichip sound card"), tr("Vass Sándor's card: SN76489, SAA1099 and YM3812 (OPL2) on the TV's sound")},
        {CardKind::Card256, tr("TVC256k++"), tr("dikdom's card: 256 KiB fast RAM, 8 MiB paged RAM, sprites and overlay graphics, drives, a mouse, vSID")},
        {CardKind::TwoDfx, tr("2dfx graphics card"), tr("Vaczkó Károly's card: sprites, 2D and 3D graphics over the paper; not with the TVC256k++")},
        {CardKind::NanoSd, tr("NanoSD"), tr("Vass Sándor's card: an SD card through an Arduino Nano; LOAD and SAVE go to it")},
        {CardKind::Serial, tr("Serial card (RS-232)"), tr("Videoton's RS-232 card (an 8251A): BASIC's device #6")},
    };
    // A column of card names after "Slot n"; an empty slot says so, centred on the longest name shown
    const char* empty = tr("Empty");
    float longest = 0.0f;
    float slot_w = 0.0f;
    char slot[32];
    for (int s = 0; s < static_cast<int>(kCardSlots); ++s) {
        const std::string name(status.slot_cards[static_cast<std::size_t>(s)]);
        longest = std::max(longest, ImGui::CalcTextSize(name.empty() ? empty : name.c_str()).x);
        std::snprintf(slot, sizeof slot, "%s %d", tr("Slot"), s);
        slot_w = std::max(slot_w, ImGui::CalcTextSize(slot).x);
    }
    const float gap = ImGui::CalcTextSize("    ").x;
    const std::string pad(static_cast<std::size_t>(std::ceil((slot_w + gap + longest) / ImGui::CalcTextSize(" ").x)), ' ');
    for (int s = 0; s < static_cast<int>(kCardSlots); ++s) {
        const CardKind held = status.slot_kinds[static_cast<std::size_t>(s)];
        const std::string name(status.slot_cards[static_cast<std::size_t>(s)]);
        std::snprintf(slot, sizeof slot, "%s %d", tr("Slot"), s);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const std::string label = pad + "###slot" + std::to_string(s);
        const bool open = ImGui::BeginMenu(label.c_str());
        // The label is blank space: the slot and the card are drawn over it
        draw->AddText({at.x, at.y}, ImGui::GetColorU32(ImGuiCol_Text), slot);
        if (name.empty()) {
            const float w = ImGui::CalcTextSize(empty).x;
            draw->AddText({at.x + slot_w + gap + (longest - w) / 2.0f, at.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), empty);
        } else {
            draw->AddText({at.x + slot_w + gap, at.y}, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        }
        if (!open) {
            continue;
        }
        for (const Choice& c : choices) {
            const bool allowed = c.kind != CardKind::Hbf || s == 0;
            // The card in the slot with settings of its own opens them beside it
            if (c.kind == held && has_card_extras(held)) {
                if (ImGui::BeginMenu(c.name)) {
                    draw_card_extras(held, status, actions);
                    ImGui::EndMenu();
                }
                continue;
            }
            if (ImGui::MenuItem(c.name, nullptr, c.kind == held, allowed) && c.kind != held) {
                if (c.kind == CardKind::None) {
                    actions.slot_clear = s;
                } else {
                    actions.slot_card = c.kind;
                    actions.slot_card_to = s;
                }
            }
            if (c.tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", c.kind == CardKind::Hbf && !allowed ? tr("The HBF disk card goes in slot 0 only") : c.tip);
            }
        }
        ImGui::EndMenu();
    }
    // The HBM 32 KiB RAM card fits the 32K machine only (a board of its own in the presets)
    const Model current = status.presets[status.preset].model;
    if (current == Model::Tvc32k || current == Model::Tvc32kHbm) {
        ImGui::Separator();
        const bool hbm = current == Model::Tvc32kHbm;
        if (ImGui::MenuItem(tr("HBM 32 KiB RAM card"), nullptr, hbm)) {
            actions.preset = preset_of(status, hbm ? Model::Tvc32k : Model::Tvc32kHbm);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Videoton's RAM card for the 32K machine: 32 KiB more, as U2 and U3; the machine powers on"));
        }
    }
}

// The main board's printer port
void draw_ports_menu(const PlayStatus& status, PlayActions& actions) {
    char item[160];
    // design/machine.md "Printer port": the main board's own port; nothing plugged in by default
    ImGui::SeparatorText(tr("Printer port"));
    const std::string printer(status.printer_file);
    std::snprintf(item, sizeof item, tr("Printer to %s..."), printer.c_str());
    actions.printer_file |= ImGui::MenuItem(status.printer ? item : tr("Printer to a file..."), nullptr, status.printer, status.can_browse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("LPRINT and LLIST go to the file: a .txt gets UTF-8 text, anything else the raw bytes"));
    }
    actions.printer_unplug |= ImGui::MenuItem(tr("No printer"), nullptr, !status.printer);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("As on the real machine: an LPRINT waits after its first character until STOP (CTRL+ESC)"));
    }

}

// The cartridge port
void draw_cart_menu(const PlayStatus& status, PlayActions& actions) {
    char item[160];
    // design/media.md "Cartridges": inserting or removing one powers the machine on
    ImGui::SeparatorText(tr("Cartridge"));
    actions.cart_insert |= ImGui::MenuItem(tr("Insert cartridge..."), nullptr, false, status.can_browse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("A .crt, .rom or .bin image; the machine powers on"));
    }
    const std::string cartridge(status.cartridge);
    std::snprintf(item, sizeof item, tr("Remove %s"), cartridge.c_str());
    actions.cart_remove |= ImGui::MenuItem(cartridge.empty() ? tr("Remove cartridge") : item, nullptr, false, !cartridge.empty());
    actions.multicart_new |= ImGui::MenuItem(tr("New MultiCart image..."), nullptr, false, status.can_browse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Pick .cas programs and .crt carts, then where the image goes; it goes in and the "
                                   "machine powers on. The menu saves, deletes and renames into the image file."));
    }
    // design/cards.md "SD Dual": the cartridge takes the port; its cards come and go without powering on
    actions.toggle_sddual |= ImGui::MenuItem(tr("SD Dual cartridge"), nullptr, status.sddual);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Sebestyén Pál's SD card cartridge with VT-DOS: in BASIC, EXT 2 starts the VT-DOS command line, "
                                   "EXT 3 the file manager; LOAD and SAVE use the card. The machine powers on."));
    }
    if (status.sddual) {
        ImGui::Indent();
        for (int s = 0; s < 2; ++s) {
            const std::string name(status.sd_cards[static_cast<std::size_t>(s)]);
            std::snprintf(item, sizeof item, s == 0 ? tr("microSD card %s...") : tr("SD card %s..."), name.c_str());
            actions.sd_insert = ImGui::MenuItem(name.empty() ? (s == 0 ? tr("microSD card image...") : tr("SD card image...")) : item,
                                                nullptr, !name.empty(), status.can_browse)
                                    ? s
                                    : actions.sd_insert;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", s == 0 ? tr("A raw image of a whole card (.img), written in place; its partitions are the first drives")
                                               : tr("A raw image of a whole card (.img), written in place; a read-only file sets the write-protect switch"));
            }
            if (!name.empty()) {
                actions.sd_eject = ImGui::MenuItem(s == 0 ? tr("Take the microSD card out") : tr("Take the SD card out")) ? s : actions.sd_eject;
            }
        }
        ImGui::Unindent();
    }
    actions.sd_new |= ImGui::MenuItem(tr("New SD card image..."), nullptr, false, status.can_browse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Pick files for its root, then where the image goes: a 64 MiB card with one FAT16 partition; it goes "
                                   "into the microSD socket, with the SD Dual"));
    }
}

void draw_tape_menu(const PlayStatus& status, PlayActions& actions) {
    // design/media.md "Tape deck": the real signal, for loaders that do not use the OS
    actions.tape_insert |= ImGui::MenuItem(tr("Insert tape..."), nullptr, false, status.can_browse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("A .wav or .ttp file, or a .cas played as a tape"));
    }
    actions.tape_run |= ImGui::MenuItem(tr("Run the tape"), nullptr, false, status.tape_loaded && !status.tape_recording);
    actions.tape_play |= ImGui::MenuItem(tr("Play"), nullptr, status.tape_state == "play", status.tape_loaded && !status.tape_recording);
    actions.tape_stop |= ImGui::MenuItem(tr("Stop"), nullptr, false, status.tape_loaded && status.tape_state != "stop");
    actions.tape_rewind |= ImGui::MenuItem(tr("Rewind"), nullptr, false, status.tape_loaded);
    actions.tape_record |= ImGui::MenuItem(tr("Record to WAV..."), nullptr, status.tape_recording, status.can_browse);
    actions.tape_eject |= ImGui::MenuItem(tr("Eject tape"), nullptr, false, status.tape_loaded);
    actions.toggle_fast_tape |= ImGui::MenuItem(tr("Fast tape"), nullptr, status.fast_tape);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Run at full speed while the tape moves"));
    }
}

void draw_disk_menu(const PlayStatus& status, PlayActions& actions) {
    // design/cards.md: drives A-D of the HBF card
    char item[160];
    for (int d = 0; d < static_cast<int>(status.disks.size()); ++d) {
        const std::string disk(status.disks[static_cast<std::size_t>(d)]);
        ImGui::PushID(d);
        if (disk.empty()) {
            std::snprintf(item, sizeof item, tr("%c: insert disk..."), 'A' + d);
            if (ImGui::MenuItem(item, nullptr, false, status.can_browse)) {
                actions.disk_insert = d;
            }
        } else {
            std::snprintf(item, sizeof item, tr("%c: eject %s"), 'A' + d, disk.c_str());
            if (ImGui::MenuItem(item)) {
                actions.disk_eject = d;
            }
        }
        ImGui::PopID();
    }
    actions.toggle_fast_disk |= ImGui::MenuItem(tr("Fast disk"), nullptr, status.fast_disk);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Run at full speed while a drive works; the machine's own timing stays real"));
    }
}

}  // namespace

float strip_space(const Skin& skin, float scale) { return (skin.strip_height + skin.groove_width) * scale; }

void draw_strip(const PlayStatus& status, const Skin& skin, const SkinFonts& fonts, PlayActions& actions) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = status.scale;
    const float strip_h = skin.strip_height * scale;
    const float groove = skin.groove_width * scale;
    const float strip_top = viewport->Pos.y + viewport->Size.y - strip_h;
    ImGui::GetBackgroundDrawList()->AddRectFilled({viewport->Pos.x, strip_top - groove},
                                                  {viewport->Pos.x + viewport->Size.x, strip_top}, to_imgui(skin.groove));

    ImGui::SetNextWindowPos({viewport->Pos.x, strip_top});
    ImGui::SetNextWindowSize({viewport->Size.x, strip_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    constexpr ImGuiWindowFlags kStripFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##strip", nullptr, kStripFlags);
    ImGui::PopStyleVar(3);

    DeckRow strip(skin, strip_h);
    if (opens(strip.slot_ribs(scale, status.slot_cards), kCardsMenu)) {
        ImGui::OpenPopup(kCardsMenu);
    }
    const float cards_x = ImGui::GetItemRectMin().x;

    const float pad = ImGui::GetStyle().FramePadding.x;
    const float strip_ratio = skin.font_strip_size > 0.0f ? skin.font_strip_size / skin.font_ui_size : 1.0f;
    const float ui_px = skin.font_ui_size * strip_ratio * scale;
    const float data_px = skin.font_data_size * strip_ratio * scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The cartridge port, beside the slots: its name after the key while one is in
    std::string cart_name(status.sddual ? std::string_view{"SD Dual"} : status.cartridge);
    if (const std::size_t dot = cart_name.rfind('.'); !status.sddual && dot != std::string::npos && dot > 0) {
        cart_name.resize(dot);
    }
    const bool cart_in = !cart_name.empty();
    if (opens(strip.cartridge("##cart", fonts.ui, ui_px, cart_in ? cart_name.c_str() : tr("Cart"), cart_in ? skin.label : skin.muted),
              kCartMenu)) {
        ImGui::OpenPopup(kCartMenu);
    }
    const float cart_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %s", tr("Cartridge"), cart_in ? cart_name.c_str() : tr("empty"));
    }
    strip.divider();
    const std::string preset(status.presets.empty() ? std::string_view{} : status.presets[status.preset].name);
    if (opens(strip.tvc("##preset", skin.label), kMachineMenu)) {
        ImGui::OpenPopup(kMachineMenu);
    }
    const float machine_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %s", tr("Machine and ROMs"), preset.c_str());
    }

    strip.divider();
    const bool symbolic = status.mapping == KeyMapping::Symbolic;
    if (opens(strip.keyboard("##mapping", skin.label), kKeysMenu)) {
        ImGui::OpenPopup(kKeysMenu);
    }
    const float keys_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", symbolic ? tr("symbolic keys") : tr("positional keys"));
    }

    strip.divider();
    const char* sound = !status.audio ? tr("no sound") : status.muted ? tr("muted") : tr("sound");
    if (strip.speaker("##sound", status.audio && !status.muted ? skin.label : skin.muted, !status.audio || status.muted,
                      skin.role(Role::Exec).on_case) &&
        status.audio) {
        actions.toggle_mute = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", sound);
    }
    strip.divider();

    // The drive light: in the notice colour while the controller works with a drive; the item opens the Disk menu
    float disk_x = 0.0f;
    {
        Colour colour = status.disk_busy >= 0 ? skin.role(Role::Notice).on_case : skin.label;
        if (!status.disk_card) {
            colour = skin.muted;
            colour.a = static_cast<std::uint8_t>(colour.a * 0.45f);
        }
        bool disk_hovered = false;
        if (opens(strip.floppy("##disk", colour), kDiskMenu)) {
            ImGui::OpenPopup(kDiskMenu);
        }
        disk_x = ImGui::GetItemRectMin().x;
        disk_hovered |= ImGui::IsItemHovered();
        if (status.disk_busy >= 0) {
            char drive[8];
            std::snprintf(drive, sizeof drive, "%c:", 'A' + status.disk_busy);
            if (opens(strip.button("##disk_drive", drive, fonts.data, data_px, colour), kDiskMenu)) {
                ImGui::OpenPopup(kDiskMenu);
            }
            disk_hovered |= ImGui::IsItemHovered();
        }
        if (disk_hovered && status.disk_card) {
            std::string drives;
            for (std::size_t d = 0; d < status.disks.size(); ++d) {
                drives += std::string(1, static_cast<char>('A' + d)) + ": " +
                          (status.disks[d].empty() ? std::string(tr("empty")) : std::string(status.disks[d])) + "\n";
            }
            drives.pop_back();
            ImGui::SetTooltip("%s", drives.c_str());
        }
        strip.divider();
    }

    // The deck: the cassette (a wave while the tape moves) opens the Tape menu; then play with the counter, stop, rewind
    {
        Colour icon = skin.label;
        if (!status.tape_loaded) {
            icon = skin.muted;
            icon.a = static_cast<std::uint8_t>(icon.a * 0.45f);
        } else if (status.tape_moving) {
            icon = skin.role(Role::Notice).on_case;
        }
        const bool clicked = status.tape_moving ? strip.tape_wave("##tape_icon", icon, status.tape_wave) : strip.cassette("##tape_icon", icon);
        if (opens(clicked, kTapeMenu)) {
            ImGui::OpenPopup(kTapeMenu);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", !status.tape_loaded ? tr("no tape")
                                    : status.tape_moving ? tr("The tape moves: PLAY or RECORD is down and the motor is on")
                                                         : tr("The tape deck; the ROM turns the motor on and off"));
        }
    }
    const float tape_x = ImGui::GetItemRectMin().x;
    const Colour active = skin.role(Role::Notice).on_case;
    const bool playing = status.tape_state == "play";
    if (strip.transport("##tape_play", false, !status.tape_loaded ? skin.rib : playing ? active : skin.label) && status.tape_loaded && !playing) {
        actions.tape_play = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", status.tape_loaded ? tr("Play") : tr("no tape"));
    }
    if (status.tape_loaded) {
        const std::string counter(status.tape_counter);
        strip.text(counter.c_str(), fonts.data, data_px, status.tape_moving ? active : skin.label);
    }
    const bool can_stop = status.tape_loaded && status.tape_state != "stop";
    if (strip.transport("##tape_stop", true, can_stop ? skin.label : skin.rib) && can_stop) {
        actions.tape_stop = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Stop"));
    }
    if (strip.rewind("##tape_rewind", status.tape_loaded ? skin.label : skin.rib) && status.tape_loaded) {
        actions.tape_rewind = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Rewind"));
    }

    strip.divider();

    const float last_x = ImGui::GetItemRectMax().x;

    // Nameplate at the right end: "free" in the muted colour, "TVC" like the case lettering
    const ImVec2 free_size = fonts.ui->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, "free");
    const ImVec2 tvc_size = fonts.strong->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, "TVC");
    const ImVec2 plate_size{free_size.x + tvc_size.x + pad * 4.0f, strip_h};
    const ImVec2 plate_pos{std::max(last_x, viewport->Pos.x + viewport->Size.x - plate_size.x), strip_top};
    ImGui::SetCursorScreenPos(plate_pos);
    if (opens(ImGui::InvisibleButton("##nameplate", plate_size), kMainMenu)) {
        ImGui::OpenPopup(kMainMenu);
    }
    strip.draw_hover(plate_pos, plate_size);
    const float text_y = plate_pos.y + (strip_h - free_size.y) / 2.0f;
    draw->AddText(fonts.ui, ui_px, {plate_pos.x + pad * 2.0f, text_y}, to_imgui(skin.muted), "free");
    draw->AddText(fonts.strong, ui_px, {plate_pos.x + pad * 2.0f + free_size.x, text_y}, to_imgui(skin.label), "TVC");
    draw->AddLine({plate_pos.x, strip_top + strip_h * 0.25f}, {plate_pos.x, strip_top + strip_h * 0.75f}, to_imgui(skin.rib));
    // The speed, left of the nameplate: a click pauses or resumes
    char state[32];
    if (status.paused) {
        std::snprintf(state, sizeof state, "%s", tr("paused"));
    } else if (status.speed_percent < 0.0) {
        std::snprintf(state, sizeof state, "%s", tr("running"));
    } else {
        std::snprintf(state, sizeof state, "%3.0f %%", status.speed_percent);
    }
    const Colour state_colour = status.paused ? skin.role(Role::Notice).on_case : skin.label;
    ImFont* const state_font = status.paused ? fonts.ui : fonts.data;
    const float state_px = status.paused ? ui_px : data_px;
    const float state_w = state_font->CalcTextSizeA(state_px, FLT_MAX, 0.0f, state).x + pad * 2.0f;
    const float state_x = plate_pos.x - state_w;
    actions.toggle_pause |= strip.button("##state", state, state_font, state_px, state_colour, true, state_x - ImGui::GetWindowPos().x);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", status.paused ? tr("Paused: click or Pause to resume")
                                              : tr("Speed in percent of a real TVC: click or Pause to pause"));
    }
    draw->AddLine({state_x, strip_top + strip_h * 0.25f}, {state_x, strip_top + strip_h * 0.75f}, to_imgui(skin.rib));
    // REC and the camera left of the speed: a click acts, a right click opens their menu
    char time[16];
    const int seconds = static_cast<int>(status.record_seconds);
    std::snprintf(time, sizeof time, "%02d:%02d", seconds / 60, seconds % 60);
    const float key_w = rec_width(fonts.strong, ui_px * 0.8f, strip_h);
    const float time_w = status.recording ? fonts.data->CalcTextSizeA(data_px, FLT_MAX, 0.0f, time).x + pad * 2.0f : 0.0f;
    const float printer_w = 52.0f * strip_h / 45.0f;
    const float media_x = state_x - printer_w - key_w * 2.0f - time_w;
    ImGui::SameLine(media_x - ImGui::GetWindowPos().x, 0.0f);
    ImGui::Dummy({0.0f, strip_h});
    // The printer: faded while none is on the port, in the notice colour while it prints
    Colour printer_colour = status.printing ? skin.role(Role::Notice).on_case : skin.label;
    if (!status.printer) {
        printer_colour = skin.muted;
        printer_colour.a = static_cast<std::uint8_t>(printer_colour.a * 0.45f);
    }
    if (opens(strip.printer("##printer", printer_colour), kPrinterMenu)) {
        ImGui::OpenPopup(kPrinterMenu);
    }
    const float printer_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemHovered()) {
        const std::string file(status.printer_file);
        ImGui::SetTooltip("%s", !status.printer ? tr("No printer") : file.empty() ? tr("Printer port") : file.c_str());
    }
    const Colour red = skin.role(Role::Exec).on_case;
    const bool lit = !status.recording || std::fmod(status.record_seconds, 1.0) < 0.6;
    actions.toggle_record |= strip.rec("##rec", fonts.strong, ui_px * 0.8f, status.recording ? red : skin.label, red, lit);
    const float rec_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(kRecordMenu);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", status.recording ? tr("Recording: click to stop; right click for the menu")
                                                 : tr("Record a video of the TV picture and sound: click to start; right click for the menu"));
    }
    if (status.recording) {
        actions.toggle_record |= strip.button("##rec_time", time, fonts.data, data_px, red);
    }
    actions.screenshot |= strip.camera("##shot", fonts.strong, ui_px * 0.8f, skin.label);
    const float shot_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(kShotMenu);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Screenshot of the TV picture as a PNG: click to take one; right click for the menu"));
    }
    draw->AddLine({media_x, strip_top + strip_h * 0.25f}, {media_x, strip_top + strip_h * 0.75f}, to_imgui(skin.rib));

    if (!status.notice.empty()) {
        const double lifetime = status.notice_is_error ? kErrorSeconds : kNoticeSeconds;
        const float alpha = static_cast<float>(lifetime - status.notice_age);
        if (alpha > 0.0f) {
            // Right-aligned after the strip's last item (the cursor has wrapped to the next line by now); too long: cut, whole on hover
            const std::string notice(status.notice);
            const float left = last_x + pad;
            const float right = media_x - pad * 2.0f;
            std::string shown = notice;
            if (ImGui::CalcTextSize(shown.c_str()).x > right - left) {
                const float dots = ImGui::CalcTextSize("...").x;
                while (!shown.empty() && ImGui::CalcTextSize(shown.c_str()).x + dots > right - left) {
                    while (shown.size() > 1 && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) {
                        shown.pop_back();  // a UTF-8 character goes whole
                    }
                    shown.pop_back();
                }
                shown += "...";
            }
            const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
            const ImVec2 at{std::max(left, right - size.x), strip_top + (strip_h - size.y) / 2.0f};
            draw->PushClipRect({left, strip_top}, viewport->Pos + viewport->Size, true);
            draw->AddText(at, with_alpha(skin.role(Role::Notice).on_case, alpha), shown.c_str());
            draw->PopClipRect();
            if (shown != notice && ImGui::IsMouseHoveringRect(at, {at.x + size.x, at.y + size.y}, false)) {
                ImGui::SetTooltip("%s", notice.c_str());
            }
        }
    }

    const float menu_top = strip_top - groove;
    // The strip's menus in the strip's text size
    ImGui::PushFont(nullptr, skin.font_ui_size * strip_ratio);
    if (begin_menu(kMainMenu, plate_pos.x + plate_size.x, menu_top, actions, 1.0f)) {
        draw_main_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kMachineMenu, machine_x, menu_top, actions)) {
        draw_machine_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kCardsMenu, cards_x, menu_top, actions)) {
        draw_cards_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kCartMenu, cart_x, menu_top, actions)) {
        draw_cart_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kPrinterMenu, printer_x, menu_top, actions)) {
        draw_ports_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kKeysMenu, keys_x, menu_top, actions)) {
        ImGui::SeparatorText(tr("Keyboard"));
        if (ImGui::MenuItem(tr("Symbolic"), nullptr, status.mapping == KeyMapping::Symbolic)) {
            actions.mapping = static_cast<int>(KeyMapping::Symbolic);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("PC characters type the same TVC characters: click for positional"));
        }
        if (ImGui::MenuItem(tr("Positional"), nullptr, status.mapping == KeyMapping::Positional)) {
            actions.mapping = static_cast<int>(KeyMapping::Positional);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("PC keys sit where the Hungarian TVC legends are: click for symbolic"));
        }
        if (ImGui::BeginMenu(tr("Key layout"))) {
            for (const KeymapEntry& entry : status.layouts) {
                if (ImGui::MenuItem(entry.name.c_str(), nullptr, entry.name == status.layout)) {
                    actions.layout = entry.path.string();
                }
            }
            ImGui::Separator();
            actions.edit_layout |= ImGui::MenuItem(tr("Edit the layout..."));
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    if (begin_menu(kTapeMenu, tape_x, menu_top, actions)) {
        draw_tape_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kDiskMenu, disk_x, menu_top, actions)) {
        draw_disk_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kRecordMenu, rec_x, menu_top, actions)) {
        ImGui::SeparatorText(tr("Video recording"));
        actions.toggle_record |= ImGui::MenuItem(status.recording ? tr("Stop recording") : tr("Start recording"));
        actions.video_dir |= ImGui::MenuItem(tr("Recordings folder..."), nullptr, false, status.can_browse);
        actions.open_video_dir |= ImGui::MenuItem(tr("Open the recordings folder"));
        ImGui::EndPopup();
    }
    if (begin_menu(kShotMenu, shot_x, menu_top, actions)) {
        ImGui::SeparatorText(tr("Screenshot"));
        actions.screenshot |= ImGui::MenuItem(tr("Take a screenshot"));
        actions.screenshot_dir |= ImGui::MenuItem(tr("Screenshots folder..."), nullptr, false, status.can_browse);
        actions.open_screenshot_dir |= ImGui::MenuItem(tr("Open the screenshots folder"));
        ImGui::EndPopup();
    }
    ImGui::PopFont();
    ImGui::End();
}

PlayActions draw_play(const PlayStatus& status, const Skin& skin, const SkinFonts& fonts) {
    PlayActions actions;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = skin.screen_margin * status.scale;
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilled(viewport->Pos, viewport->Pos + viewport->Size, to_imgui(skin.frame));
    if (status.screen != ImTextureID_Invalid) {
        const ImVec2 area{viewport->Size.x - status.side_panel - margin * 2.0f, viewport->Size.y - strip_space(skin, status.scale) - status.bottom_panel - margin * 2.0f};
        const ScreenRect shown = draw_screen(background, status.screen, status.full_raster, viewport->Pos + ImVec2{margin, margin}, area,
                                             skin.label, &status.picture);
        // Recording: a blinking red dot in the picture's top right corner (on the window only, not in the video)
        if (status.recording && std::fmod(status.record_seconds, 1.0) < 0.6) {
            const float r = std::max(4.0f, shown.size.y * 0.018f);
            background->AddCircleFilled({shown.min.x + shown.size.x - r * 2.5f, shown.min.y + r * 2.5f}, r,
                                        to_imgui(skin.role(Role::Exec).on_case));
        }
        if (status.splash != ImTextureID_Invalid && status.splash_alpha > 0.0f && status.splash_size.x > 0.0f) {
            const float a = status.splash_alpha;
            background->AddRectFilled(shown.min, shown.min + shown.size, IM_COL32(0, 0, 0, static_cast<int>(a * 255.0f)));
            const float fit = std::min(shown.size.x * 0.8f / status.splash_size.x, shown.size.y * 0.8f / status.splash_size.y);
            const ImVec2 size{status.splash_size.x * fit, status.splash_size.y * fit};
            const ImVec2 p0{shown.min.x + (shown.size.x - size.x) * 0.5f, shown.min.y + (shown.size.y - size.y) * 0.5f};
            background->AddImage(status.splash, p0, p0 + size, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, static_cast<int>(a * 0.45f * 255.0f)));
        }
    }
    draw_strip(status, skin, fonts, actions);
    return actions;
}

}  // namespace freetvc::sdl
