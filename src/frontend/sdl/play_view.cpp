// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/play_view.hpp"

#include <algorithm>
#include <cfloat>
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

// Opens `id` above the strip at window x `anchor`; true while it is open (EndPopup is the caller's)
bool begin_menu(const char* id, float anchor, float strip_top, PlayActions& actions) {
    ImGui::SetNextWindowPos({anchor, strip_top}, ImGuiCond_Appearing, {0.0f, 1.0f});
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
    ImGui::SeparatorText(tr("Program"));
    actions.load_program |= ImGui::MenuItem(tr("Load program..."), nullptr, false, status.can_browse);
    const std::string cassette(status.cassette);
    char item[160];
    std::snprintf(item, sizeof item, tr("Run %s again"), cassette.c_str());
    actions.run_again |= ImGui::MenuItem(cassette.empty() ? tr("Run again") : item, nullptr, false, !cassette.empty());
    std::snprintf(item, sizeof item, tr("Eject %s"), cassette.c_str());
    actions.eject |= ImGui::MenuItem(cassette.empty() ? tr("Eject cassette") : item, nullptr, false, !cassette.empty());

    // design/media.md "Snapshots": the whole machine, with the tape and cassette by path
    ImGui::SeparatorText(tr("Snapshot"));
    actions.quick_save |= ImGui::MenuItem(tr("Quick save"), "F9");
    actions.quick_load |= ImGui::MenuItem(tr("Quick load"), "Shift+F9", false, status.quick_snapshot);
    actions.snapshot_save |= ImGui::MenuItem(tr("Save snapshot..."), nullptr, false, status.can_browse);
    actions.snapshot_load |= ImGui::MenuItem(tr("Load snapshot..."), nullptr, false, status.can_browse);

    ImGui::SeparatorText(tr("Keyboard"));
    if (ImGui::MenuItem(tr("Symbolic"), nullptr, status.mapping == KeyMapping::Symbolic)) {
        actions.mapping = static_cast<int>(KeyMapping::Symbolic);
    }
    if (ImGui::MenuItem(tr("Positional"), nullptr, status.mapping == KeyMapping::Positional)) {
        actions.mapping = static_cast<int>(KeyMapping::Positional);
    }

    ImGui::SeparatorText(tr("Skin"));
    for (const SkinEntry& entry : status.skins) {
        if (ImGui::MenuItem(entry.name.c_str(), nullptr, entry.name == status.skin_name)) {
            actions.skin = entry.path.string();
        }
    }
    actions.reload_skin |= ImGui::MenuItem(tr("Reload skin"));

    // Each language under its own name
    ImGui::SeparatorText(tr("Language"));
    if (ImGui::MenuItem("English", nullptr, status.language == "en")) {
        actions.language = "en";
    }
    for (const LanguageFile& file : status.languages) {
        if (ImGui::MenuItem(file.name.c_str(), nullptr, status.language == file.code)) {
            actions.language = file.code;
        }
    }

    ImGui::SeparatorText(tr("Screen"));
    actions.toggle_full_raster |= ImGui::MenuItem(tr("Full raster (debug)"), nullptr, status.full_raster);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Show the whole beam path, sync and blanking included; the TV area is outlined"));
    }
    actions.screenshot |= ImGui::MenuItem(tr("Screenshot"));
    actions.toggle_fullscreen |= ImGui::MenuItem(tr("Full screen"), "F11", status.fullscreen);

    ImGui::Separator();
    actions.toggle_settings |= ImGui::MenuItem(tr("Settings..."), nullptr, status.settings);
    actions.toggle_bench |= ImGui::MenuItem(tr("Bench"), "F12", status.bench);
    actions.open_about |= ImGui::MenuItem(tr("About freeTVC..."));
    actions.quit |= ImGui::MenuItem(tr("Quit"));
}

void draw_machine_menu(const PlayStatus& status, PlayActions& actions) {
    ImGui::SeparatorText(tr("Machine"));
    for (std::size_t i = 0; i < status.presets.size(); ++i) {
        const std::string name(status.presets[i].name);
        if (ImGui::MenuItem(name.c_str(), nullptr, i == status.preset) && i != status.preset) {
            actions.preset = static_cast<int>(i);
        }
    }
    actions.fetch_roms |= ImGui::MenuItem(tr("Download ROMs"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("The TVC's ROMs from tvc.homeserver.hu and the cards' firmware from their authors' sites, with the system's curl, into the ROM folder chosen in Settings (rom_dir), else the user data folder"));
    }
    ImGui::Separator();
    actions.toggle_pause |= ImGui::MenuItem(tr("Pause"), "Pause", status.paused);
    actions.reset |= ImGui::MenuItem(tr("Reset"));
    actions.power_cycle |= ImGui::MenuItem(tr("Power cycle"));
}

// The slot a card goes into, or moves to; a slot with another card shows it, greyed out
void slot_choice(const PlayStatus& status, CardKind kind, int slot, const char* id, PlayActions& actions) {
    ImGui::Indent();
    const std::string label = std::string(slot >= 0 ? tr("Move to slot") : tr("Put in slot")) + "##" + id;
    if (ImGui::BeginMenu(label.c_str())) {
        char item[160];
        for (int s = 0; s < static_cast<int>(kCardSlots); ++s) {
            const bool free = status.slot_cards[s].empty();
            if (free || s == slot) {
                std::snprintf(item, sizeof item, tr("Slot %d, ports %d0h-%dFh"), s, s + 1, s + 1);
            } else {
                const std::string holder(status.slot_cards[s]);
                std::snprintf(item, sizeof item, tr("Slot %d, ports %d0h-%dFh: %s"), s, s + 1, s + 1, holder.c_str());
            }
            if (ImGui::MenuItem(item, nullptr, s == slot, free || s == slot) && s != slot) {
                actions.slot_card = kind;
                actions.slot_card_to = s;
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Unindent();
}

void draw_cards_menu(const PlayStatus& status, PlayActions& actions) {
    // design/cards.md: switching a card powers the machine on
    ImGui::SeparatorText(tr("Cards"));
    actions.toggle_disk_card |= ImGui::MenuItem(tr("Disk card in slot 0"), nullptr, status.disk_card);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("HBF card in slot 0: LOAD and SAVE use the disks, not the cassette; the machine powers on"));
    }
    char item[160];
    if (status.gamecard_slot >= 0) {
        std::snprintf(item, sizeof item, tr("GameCard in slot %d"), status.gamecard_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("GameCard"));
    }
    actions.toggle_gamecard |= ImGui::MenuItem(item, nullptr, status.gamecard_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("dikdom's card: SN76489 sound, joysticks 3 and 4 on gamepads 3 and 4, a boot menu with four games; "
                                   "the machine powers on"));
    }
    slot_choice(status, CardKind::GameCard, status.gamecard_slot, "gamecard", actions);
    if (status.multichip_slot >= 0) {
        std::snprintf(item, sizeof item, tr("Multichip sound card in slot %d"), status.multichip_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("Multichip sound card"));
    }
    actions.toggle_multichip |= ImGui::MenuItem(item, nullptr, status.multichip_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Vass Sándor's card: SN76489, SAA1099 and YM3812 (OPL2) on the TV's sound; its start-up screen plays "
                                   "a demo on each chip after SPACE; the machine powers on"));
    }
    slot_choice(status, CardKind::Multichip, status.multichip_slot, "multichip", actions);
    // Outside the slots, so no slot number; one chip at a time
    for (const int chip : {1, 2}) {
        if (ImGui::MenuItem(chip == 1 ? tr("SID card, MOS 6581") : tr("SID card, MOS 8580"), nullptr, status.sid_card == chip)) {
            actions.sid_card = status.sid_card == chip ? 0 : chip;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A SID chip at ports E0h-FFh, outside the slots, where WinTVC puts it (no real card's documentation "
                                       "is known); reSID on the TV's sound; the machine powers on"));
        }
    }
    if (status.card256_slot >= 0) {
        std::snprintf(item, sizeof item, tr("TVC256k++ in slot %d"), status.card256_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("TVC256k++"));
    }
    actions.toggle_card256 |= ImGui::MenuItem(item, nullptr, status.card256_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("dikdom's card: 256 KiB fast RAM in place of U2/U3, 8 MiB paged RAM, sprites and overlay graphics, "
                                   "drives for LOAD and SAVE, a mouse, its EXT command line (EXT and the slot number), its vSID sound; "
                                   "the machine powers on"));
    }
    slot_choice(status, CardKind::Card256, status.card256_slot, "card256", actions);
    if (status.card256_slot >= 0) {
        // One USB port, so the pendrive and the mouse take turns; none of this powers the machine on
        ImGui::Indent();
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
        ImGui::Unindent();
    }
    if (status.twodfx_slot >= 0) {
        std::snprintf(item, sizeof item, tr("2dfx graphics card in slot %d"), status.twodfx_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("2dfx graphics card"));
    }
    actions.toggle_twodfx |= ImGui::MenuItem(item, nullptr, status.twodfx_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Vaczkó Károly's card: sprites, 2D and 3D graphics over the paper, ports F8h/F9h (a SID card keeps "
                                   "E0h-F7h); not with the TVC256k++; the machine powers on"));
    }
    slot_choice(status, CardKind::TwoDfx, status.twodfx_slot, "twodfx", actions);
    if (status.nanosd_slot >= 0) {
        std::snprintf(item, sizeof item, tr("NanoSD in slot %d"), status.nanosd_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("NanoSD"));
    }
    actions.toggle_nanosd |= ImGui::MenuItem(item, nullptr, status.nanosd_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Vass Sándor's card: an SD card through an Arduino Nano; LOAD and SAVE go to it, a file-select menu at "
                                   "power-on (ESC skips it), its command line with EXT 0; the machine powers on"));
    }
    slot_choice(status, CardKind::NanoSd, status.nanosd_slot, "nanosd", actions);
    if (status.nanosd_slot >= 0) {
        // The SD card comes and goes like on the card, without powering on; the Nano notices
        ImGui::Indent();
        const std::string sd(status.nanosd_sd);
        std::snprintf(item, sizeof item, tr("SD card %s..."), sd.c_str());
        actions.nanosd_sd |= ImGui::MenuItem(sd.empty() ? tr("SD card folder...") : item, nullptr, !sd.empty(), status.can_browse);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("A host folder as the SD card: the card sees 8.3 names only, files are written at once"));
        }
        actions.nanosd_eject |= ImGui::MenuItem(tr("Take the SD card out"), nullptr, false, !sd.empty());
        ImGui::Unindent();
    }
    if (status.serial_slot >= 0) {
        std::snprintf(item, sizeof item, tr("Serial card in slot %d"), status.serial_slot);
    } else {
        std::snprintf(item, sizeof item, "%s", tr("Serial card (RS-232)"));
    }
    actions.toggle_serial |= ImGui::MenuItem(item, nullptr, status.serial_slot >= 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Videoton's RS-232 card (an 8251A): BASIC's device #6 (LIST #6, SAVE #6, LOAD #6); the machine powers on"));
    }
    slot_choice(status, CardKind::Serial, status.serial_slot, "serial", actions);
    if (status.serial_slot >= 0) {
        // The cable and its files change without powering on
        ImGui::Indent();
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
        ImGui::Unindent();
    }

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
    ImGui::SeparatorText(tr("Tape"));
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
    ImGui::SeparatorText(tr("Disk"));
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

    // Nameplate: "free" in the muted colour, "TVC" like the case lettering
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float ui_px = skin.font_ui_size * scale;
    const float data_px = skin.font_data_size * scale;
    const ImVec2 free_size = fonts.ui->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, "free");
    const ImVec2 tvc_size = fonts.strong->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, "TVC");
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 plate_pos = ImGui::GetCursorScreenPos();
    const ImVec2 plate_size{free_size.x + tvc_size.x + pad * 2.0f, strip_h};
    if (opens(ImGui::InvisibleButton("##nameplate", plate_size), kMainMenu)) {
        ImGui::OpenPopup(kMainMenu);
    }
    strip.draw_hover(plate_pos, plate_size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float text_y = plate_pos.y + (strip_h - free_size.y) / 2.0f;
    draw->AddText(fonts.ui, ui_px, {plate_pos.x + pad, text_y}, to_imgui(skin.muted), "free");
    draw->AddText(fonts.strong, ui_px, {plate_pos.x + pad + free_size.x, text_y}, to_imgui(skin.label), "TVC");

    strip.divider();
    const std::string preset(status.presets.empty() ? std::string_view{} : status.presets[status.preset].name);
    if (opens(strip.button("##preset", preset.c_str(), fonts.ui, ui_px, skin.label), kMachineMenu)) {
        ImGui::OpenPopup(kMachineMenu);
    }
    const float machine_x = ImGui::GetItemRectMin().x;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Machine and ROMs"));
    }

    strip.divider();
    char state[32];
    if (status.paused) {
        std::snprintf(state, sizeof state, "%s", tr("paused"));
    } else if (status.speed_percent < 0.0) {
        std::snprintf(state, sizeof state, "%s", tr("running"));
    } else {
        std::snprintf(state, sizeof state, "%3.0f %%", status.speed_percent);
    }
    const Colour state_colour = status.paused ? skin.role(Role::Notice).on_case : skin.label;
    actions.toggle_pause |= status.paused ? strip.button("##state", state, fonts.ui, ui_px, state_colour)
                                          : strip.button("##state", state, fonts.data, data_px, state_colour);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", status.paused ? tr("Paused: click or Pause to resume")
                                              : tr("Speed in percent of a real TVC: click or Pause to pause"));
    }

    strip.divider();
    const bool symbolic = status.mapping == KeyMapping::Symbolic;
    if (strip.button("##mapping", symbolic ? tr("symbolic keys") : tr("positional keys"), fonts.ui, ui_px, skin.label)) {
        actions.mapping = static_cast<int>(symbolic ? KeyMapping::Positional : KeyMapping::Symbolic);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", symbolic ? tr("PC characters type the same TVC characters: click for positional")
                                         : tr("PC keys sit where the Hungarian TVC legends are: click for symbolic"));
    }

    strip.divider();
    const char* sound = !status.audio ? tr("no sound") : status.muted ? tr("muted") : tr("sound");
    if (strip.button("##sound", sound, fonts.ui, ui_px, status.audio && !status.muted ? skin.label : skin.muted) && status.audio) {
        actions.toggle_mute = true;
    }
    strip.divider();

    // The play/stop key, before the deck: play with no tape picks one first
    const bool stop_key = status.tape_loaded && status.tape_state != "stop";
    if (strip.transport("##tape_key", stop_key, stop_key && status.tape_moving ? skin.role(Role::Notice).on_case : skin.label)) {
        if (stop_key) {
            actions.tape_stop = true;
        } else if (status.tape_loaded) {
            actions.tape_play = true;
        } else if (status.can_browse) {
            actions.tape_insert_play = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", stop_key ? tr("Stop") : status.tape_loaded ? tr("Play") : tr("Insert a tape and play it"));
    }

    // The deck, always there: state and counter, in the notice colour while the tape moves; the item opens the Tape menu
    if (status.tape_loaded) {
        char tape[48];
        std::snprintf(tape, sizeof tape, "%s %.*s %.*s", tr("tape"), static_cast<int>(status.tape_state.size()), status.tape_state.data(),
                      static_cast<int>(status.tape_counter.size()), status.tape_counter.data());
        const Colour colour = status.tape_moving ? skin.role(Role::Notice).on_case : skin.label;
        if (opens(strip.button("##tape", tape, fonts.data, data_px, colour), kTapeMenu)) {
            ImGui::OpenPopup(kTapeMenu);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", status.tape_moving ? tr("The tape moves: PLAY or RECORD is down and the motor is on")
                                                       : tr("The tape deck; the ROM turns the motor on and off"));
        }
    } else if (opens(strip.button("##tape", tr("no tape"), fonts.ui, ui_px, skin.muted), kTapeMenu)) {
        ImGui::OpenPopup(kTapeMenu);
    }
    const float tape_x = ImGui::GetItemRectMin().x;
    strip.divider();

    // The drive light: in the notice colour while the controller works with a drive; the item opens the Disk menu
    float disk_x = tape_x;
    if (status.disk_card) {
        char disk[32];
        if (status.disk_busy >= 0) {
            std::snprintf(disk, sizeof disk, "%s %c:", tr("disk"), 'A' + status.disk_busy);
        } else {
            std::snprintf(disk, sizeof disk, "%s", tr("disk"));
        }
        if (opens(strip.button("##disk", disk, fonts.data, data_px, status.disk_busy >= 0 ? skin.role(Role::Notice).on_case : skin.label),
                  kDiskMenu)) {
            ImGui::OpenPopup(kDiskMenu);
        }
        disk_x = ImGui::GetItemRectMin().x;
        if (ImGui::IsItemHovered()) {
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

    if (!status.notice.empty()) {
        const double lifetime = status.notice_is_error ? kErrorSeconds : kNoticeSeconds;
        const float alpha = static_cast<float>(lifetime - status.notice_age);
        if (alpha > 0.0f) {
            // Right-aligned after the strip's last item (the cursor has wrapped to the next line by now); too long: cut, whole on hover
            const std::string notice(status.notice);
            const float left = ImGui::GetItemRectMax().x + pad;
            const float right = viewport->Pos.x + viewport->Size.x - pad * 2.0f;
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
    if (begin_menu(kMainMenu, plate_pos.x, menu_top, actions)) {
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
    if (begin_menu(kTapeMenu, tape_x, menu_top, actions)) {
        draw_tape_menu(status, actions);
        ImGui::EndPopup();
    }
    if (begin_menu(kDiskMenu, disk_x, menu_top, actions)) {
        draw_disk_menu(status, actions);
        ImGui::EndPopup();
    }
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
        draw_screen(background, status.screen, status.full_raster, viewport->Pos + ImVec2{margin, margin}, area, skin.label, &status.picture);
    }
    draw_strip(status, skin, fonts, actions);
    return actions;
}

}  // namespace freetvc::sdl
