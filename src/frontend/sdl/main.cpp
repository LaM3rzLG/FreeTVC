// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 + Dear ImGui front end: Play and Bench modes with skins (design/ui.md).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "core/cards/card256.hpp"
#include "core/cards/multichip.hpp"
#include "core/cards/nanosd.hpp"
#include "core/machine/machine.hpp"
#include "core/version.hpp"
#include "debug/debugger.hpp"
#include "frontend/common/host_tape.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/common/rom_fetch.hpp"
#include "frontend/common/settings.hpp"
#include "frontend/common/video_recorder.hpp"
#include "frontend/sdl/bench_view.hpp"
#include "frontend/sdl/host_input.hpp"
#include "frontend/sdl/play_view.hpp"
#include "frontend/sdl/display.hpp"
#include "frontend/sdl/keyboard_editor.hpp"
#include "frontend/sdl/keymap.hpp"
#include "frontend/sdl/picture.hpp"
#include "frontend/sdl/about_view.hpp"
#include "frontend/sdl/settings_view.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "frontend/sdl/text.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "remote/dzrp.hpp"
#include "remote/dzrp_host.hpp"
#include "remote/gdb.hpp"
#include "remote/gdb_host.hpp"
#include "remote/mcp.hpp"
#include "remote/mcp_host.hpp"
#include "remote/monitor.hpp"
#include "remote/monitor_hub.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kTicksPerSecond = 6'250'000;
constexpr std::uint64_t kMaxCatchUp = 2 * freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
constexpr std::uint64_t kSkinPollNs = 500'000'000;

void upload_frame(freetvc::sdl::Display& display, ImTextureID texture, const freetvc::Framebuffer& frame) {
    static std::vector<std::uint32_t> pixels(static_cast<std::size_t>(freetvc::Framebuffer::kWidth) * freetvc::Framebuffer::kHeight);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const std::uint8_t index = frame.pixels[i];
        pixels[i] = 0xFF000000u | freetvc::kRgbPalette[index < freetvc::Framebuffer::kColours ? index : 0];
    }
    display.update_texture(texture, pixels.data(), freetvc::Framebuffer::kWidth * 4);
}

// The picture file in use, watched like the skin (design/ui.md "Picture")
struct PictureState {
    freetvc::sdl::Picture picture;
    fs::path path;  // empty = sharp, built in
    fs::file_time_type stamp{};
    std::vector<fs::path> dirs;  // the user data folder's pictures/ first: the settings window's copies live there
    std::vector<freetvc::sdl::PictureEntry> list;

    std::string load(const fs::path& file) {
        std::string error;
        list = freetvc::sdl::list_pictures(dirs);
        std::error_code ec;
        const auto new_stamp = fs::last_write_time(file, ec);
        const auto loaded = freetvc::sdl::load_picture_file(file, error);
        if (!loaded) {
            stamp = path == file ? new_stamp : stamp;
            return error;
        }
        picture = *loaded;
        path = file;
        stamp = new_stamp;
        return {};
    }
    bool changed_on_disk() const {
        std::error_code ec;
        return !path.empty() && fs::last_write_time(path, ec) != stamp && !ec;
    }
    // A name from the list, or a path to a file
    std::optional<fs::path> find(const std::string& name_or_path) {
        list = freetvc::sdl::list_pictures(dirs);
        for (const auto& entry : list) {
            if (entry.name == name_or_path) {
                return entry.path;
            }
        }
        std::error_code ec;
        if (fs::is_regular_file(name_or_path, ec)) {
            return fs::path(name_or_path);
        }
        return std::nullopt;
    }
};

fs::path sdl_path(char* owned) {
    fs::path path = owned ? fs::path(owned) : fs::path();
    SDL_free(owned);
    return path;
}

// PNG of a framebuffer area, one pixel per framebuffer pixel, in <user data>/screenshots; returns the path, or empty and sets error
bool is_snapshot_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".tvcs" || ext == ".tvs";
}

// "<name> 2026-11-06 22-15-30<ext>": the program (or preset) and the local time
std::string timestamped(std::string_view name, const char* ext) {
    SDL_DateTime now{};
    SDL_Time ticks = 0;
    if (!SDL_GetCurrentTime(&ticks) || !SDL_TimeToDateTime(ticks, &now, true)) {
        now = {};
    }
    std::string clean(name);
    for (char& c : clean) {
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) {
            c = '_';
        }
    }
    char text[160];
    std::snprintf(text, sizeof text, "%s %04d-%02d-%02d %02d-%02d-%02d%s", clean.c_str(), now.year, now.month, now.day, now.hour,
                  now.minute, now.second, ext);
    return text;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A user folder from the OS (desktop, videos), else `fallback`
fs::path user_folder(SDL_Folder folder, const fs::path& fallback) {
    const char* dir = SDL_GetUserFolder(folder);
    return dir && *dir ? fs::path(std::u8string(reinterpret_cast<const char8_t*>(dir))) : fallback;
}

// Opens a folder in the system's file manager
void open_folder(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string url = "file://";
    for (const unsigned char c : utf8(fs::absolute(dir, ec))) {
        if (std::isalnum(c) || std::string_view("/-_.~").find(static_cast<char>(c)) != std::string_view::npos) {
            url += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
    SDL_OpenURL(url.c_str());
}

// A PNG of `area` in `dir` at the PAL aspect (4:3 for the TV area): every line 4 times, the pixels area-averaged across;
// returns its path, empty with `error` set on failure
std::string save_screenshot(const freetvc::Framebuffer& frame, freetvc::sdl::ScreenArea area, const fs::path& dir,
                            std::string_view name, std::string& error) {
    constexpr int kLineScale = 4;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const int width = freetvc::pal_width(area, kLineScale);
    SDL_Surface* surface = SDL_CreateSurface(width, area.height * kLineScale, SDL_PIXELFORMAT_XRGB8888);
    if (!surface) {
        error = SDL_GetError();
        return {};
    }
    const double step = static_cast<double>(area.width) / width;  // source pixels per output pixel
    for (int y = 0; y < area.height; ++y) {
        const std::uint8_t* src = &frame.pixels[static_cast<std::size_t>(area.y + y) * freetvc::Framebuffer::kWidth + area.x];
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(surface->pixels) + y * kLineScale * surface->pitch);
        for (int x = 0; x < width; ++x) {
            const double from = x * step;
            const double to = from + step;
            double rgb[3] = {0.0, 0.0, 0.0};
            for (int sx = static_cast<int>(from); sx < area.width && sx < to; ++sx) {
                const double weight = std::min<double>(sx + 1, to) - std::max<double>(sx, from);
                const std::uint32_t c = freetvc::kRgbPalette[src[sx] < freetvc::Framebuffer::kColours ? src[sx] : 0];
                rgb[0] += weight * (c >> 16 & 0xFF);
                rgb[1] += weight * (c >> 8 & 0xFF);
                rgb[2] += weight * (c & 0xFF);
            }
            const auto channel = [&](double v) { return static_cast<std::uint32_t>(std::min(255.0, v / step + 0.5)); };
            row[x] = channel(rgb[0]) << 16 | channel(rgb[1]) << 8 | channel(rgb[2]);
        }
        for (int k = 1; k < kLineScale; ++k) {
            std::memcpy(static_cast<std::uint8_t*>(surface->pixels) + (y * kLineScale + k) * surface->pitch, row,
                        static_cast<std::size_t>(width) * 4);
        }
    }
    const std::string path = utf8(dir / fs::path(std::u8string(reinterpret_cast<const char8_t*>(timestamped(name, ".png").c_str()))));
    const bool saved = SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    if (!saved) {
        error = SDL_GetError();
        return {};
    }
    return path;
}

// The skin in use, its file and fonts; reloads when the file changes on disk
struct SkinState {
    freetvc::sdl::Skin skin;
    freetvc::sdl::SkinFonts fonts;
    fs::path path;  // empty = built-in skin
    fs::file_time_type stamp{};
    std::vector<fs::path> dirs;
    fs::path font_dir;
    std::vector<freetvc::sdl::SkinEntry> list;
    float scale = 1.0f;

    // Applies the file's skin, or keeps the current one and returns the error
    std::string load(const fs::path& file) {
        std::string error;
        list = freetvc::sdl::list_skins(dirs);
        std::error_code ec;
        const auto new_stamp = fs::last_write_time(file, ec);
        const auto loaded = freetvc::sdl::load_skin_file(file, error);
        if (!loaded) {
            stamp = path == file ? new_stamp : stamp;  // don't retry a bad file until it changes again
            return error;
        }
        skin = *loaded;
        path = file;
        stamp = new_stamp;
        return apply();
    }

    std::string apply() {
        std::string warning;
        fonts = freetvc::sdl::load_fonts(skin, path.empty() ? font_dir : path.parent_path(), font_dir, warning);
        freetvc::sdl::apply_style(skin, scale, ImGui::GetStyle());  // after the fonts: ClearFonts() rewrites the size
        return warning;
    }

    bool changed_on_disk() const {
        std::error_code ec;
        return !path.empty() && fs::last_write_time(path, ec) != stamp && !ec;
    }
};

// SDL's file dialog may answer on another thread
struct FileChoice {
    // autostart, into the tape deck, a WAV to record into, a cartridge, a snapshot to write or to read, a disk, the
    // TVC256k++'s pendrive folder or flash image, the NanoSD's SD card, a MultiCart's programs and then its image, an
    // SD Dual card image, a new SD card's files and then its image
    enum class Purpose {
        Program, Tape, Record, Cart, SnapshotSave, SnapshotLoad, Disk, Pendrive, Flash, SdCard, MultiCartFiles, MultiCartImage,
        SdImage, SdNewFiles, SdNewImage, PrinterFile, SerialSend, SerialCapture, Symbols, RomDir, DiskRom, VideoDir, ShotDir
    };
    std::mutex mutex;
    bool showing = false;
    Purpose purpose = Purpose::Program;
    unsigned drive = 0;  // Disk: where it goes; SdImage: the socket
    bool play = false;   // Tape: PLAY goes down once it is in
    std::optional<std::string> path;
    std::vector<std::string> more;  // MultiCartFiles, SdNewFiles: the files after the first
    std::string error;
    std::vector<SDL_DialogFileFilter> filters;  // SDL reads them until the dialog answers

    // The filters with their names in the UI language
    const SDL_DialogFileFilter* translated(const SDL_DialogFileFilter* english, int count) {
        filters.assign(english, english + count);
        for (SDL_DialogFileFilter& filter : filters) {
            filter.name = freetvc::sdl::tr(filter.name);
        }
        return filters.data();
    }

    static void SDLCALL done(void* self, const char* const* files, int) {
        auto* choice = static_cast<FileChoice*>(self);
        const std::lock_guard lock(choice->mutex);
        choice->showing = false;
        if (!files) {
            choice->error = SDL_GetError();
        } else if (files[0] && files[0][0] != '\0') {  // SDL's zenity driver reports a cancel as one empty name
            choice->path = files[0];
            choice->more.clear();
            for (int i = 1; files[i]; ++i) {
                choice->more.emplace_back(files[i]);
            }
        }
    }
};

// An exec checkpoint for the instruction at `address` in the bank paged in there: removes it if one exists, else adds one
void toggle_checkpoint(freetvc::debug::Debugger& debugger, std::uint16_t address) {
    const freetvc::BankAddress where = debugger.machine().memory().locate(address);
    std::vector<unsigned> found;
    for (const freetvc::debug::Checkpoint& c : debugger.checkpoints()) {
        const std::uint16_t at = c.bank && c.offsets ? where.offset : address;
        if (c.access == freetvc::kAccessExec && c.start == at && c.end == at && (!c.bank || *c.bank == where.bank)) {
            found.push_back(c.id);
        }
    }
    for (const unsigned id : found) {
        debugger.remove(id);
    }
    if (found.empty()) {
        freetvc::debug::Checkpoint c;
        c.access = freetvc::kAccessExec;
        c.start = c.end = address;
        if (where.bank != freetvc::Bank::Open) {
            c.bank = where.bank;
        }
        debugger.add(std::move(c));
    }
}

// No ROMs at start: offers the autofetch in a message box; true once they are in `dir`
bool offer_rom_download(const fs::path& dir) {
    const freetvc::CurlCheck curl = freetvc::find_curl();
    using freetvc::sdl::tr;
    std::string text = std::string(tr("freeTVC needs the TV Computer's ROMs and found none (the places searched are on stderr).")) + "\n\n";
    if (!curl.ok()) {
        text += freetvc::sdl::tr_format("It can download them from tvc.homeserver.hu, but %s.", freetvc::sdl::translate(curl.problem).c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, tr("freeTVC: no ROMs"), text.c_str(), nullptr);
        return false;
    }
    text += freetvc::sdl::tr_format("Download them from tvc.homeserver.hu into\n%s ?\n\nEvery file is checked against a pinned SHA-256 "
                                    "sum. It takes a few seconds and uses %s.",
                                    dir.string().c_str(), curl.program.string().c_str());
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, tr("Quit")},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, tr("Download")}};
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, tr("freeTVC: no ROMs"), text.c_str(), 2, buttons, nullptr};
    int chosen = 0;
    if (!SDL_ShowMessageBox(&box, &chosen) || chosen != 1) {
        return false;
    }
    std::string error;
    const bool ok = freetvc::fetch_roms(curl, dir, error, [](const std::string& line) { std::fprintf(stderr, "freetvc: %s\n", line.c_str()); });
    if (!ok) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, tr("freeTVC: ROM download failed"), freetvc::sdl::translate(error).c_str(), nullptr);
    }
    return ok;
}

// The menu's ROM download runs on its own thread; the frame loop picks up the result
struct RomDownload {
    std::thread worker;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string result;
    fs::path dir;

    bool running() const { return worker.joinable() && !done; }
    // True once, when a started download has ended
    bool finished() {
        if (!worker.joinable() || !done) {
            return false;
        }
        worker.join();
        return true;
    }
    void start(fs::path into) {
        if (worker.joinable()) {
            worker.join();  // an ended download not reported yet
        }
        done = false;
        dir = std::move(into);
        worker = std::thread([this, target = dir] {
            const freetvc::CurlCheck curl = freetvc::find_curl();
            std::string error;
            ok = curl.ok() && freetvc::fetch_roms(curl, target, error);
            if (ok) {
                std::fprintf(stderr, "freetvc: ROMs downloaded into %s\n", target.string().c_str());
            }
            result = curl.ok() ? error : curl.problem;
            done = true;
        });
    }
    ~RomDownload() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

struct Notice {
    std::string text;
    bool error = false;
    std::uint64_t since_ns = 0;

    void set(std::string message, bool is_error) {
        if (message.empty()) {
            return;
        }
        std::fprintf(stderr, "freetvc: %s\n", message.c_str());
        text = freetvc::sdl::translate(message);
        error = is_error;
        since_ns = SDL_GetTicksNS();
    }

    // English on stderr, the UI language in the strip; messages from shared code among the arguments too
    void show_text(bool is_error, const char* english) {
        std::fprintf(stderr, "freetvc: %s\n", english);
        text = freetvc::sdl::tr(english);
        error = is_error;
        since_ns = SDL_GetTicksNS();
    }
    FREETVC_PRINTF(3, 4) void show(bool is_error, const char* english, ...) {
        std::va_list args;
        va_start(args, english);
        const std::string line = freetvc::vmsg(english, args);
        va_end(args);
        std::fprintf(stderr, "freetvc: %s\n", line.c_str());
        text = freetvc::sdl::translate(line);
        error = is_error;
        since_ns = SDL_GetTicksNS();
    }
};

// UI scale in window points: ImGui already applies the pixel density (macOS Retina, Wayland)
float ui_scale(SDL_Window* window) {
    const float display = SDL_GetWindowDisplayScale(window);
    const float density = SDL_GetWindowPixelDensity(window);
    const float scale = display > 0.0f && density > 0.0f ? display / density : 1.0f;
    return scale > 0.0f ? scale : 1.0f;
}

}  // namespace

int main(int argc, char** argv) {
    freetvc::set_message_hook(&freetvc::sdl::record_message);  // Before any message or thread
    // freetvc.ini gives the defaults; the command line overrides them for this run
    const fs::path base_dir = sdl_path(SDL_strdup(SDL_GetBasePath() ? SDL_GetBasePath() : ""));
    const fs::path user_dir = sdl_path(SDL_GetPrefPath(nullptr, "freeTVC"));
    const freetvc::SettingsFile settings = freetvc::load_settings(base_dir, user_dir);
    long frame_limit = 0;
    bool audio_wanted = settings.values.audio.value_or(true);
    bool audio_given = false;
    bool audio_at_start = audio_wanted;  // the freetvc.ini value the settings sheet shows
    int volume = settings.values.volume.value_or(100);
    bool fullscreen_at_start = settings.values.fullscreen.value_or(false);
    bool full_raster = settings.values.full_raster.value_or(false);
    std::string preset_name = settings.values.preset.value_or("64k+-2.2");
    std::string rom_dir_option;
    std::string skin_option = settings.values.skin.value_or("bronze");
    auto key_mapping = freetvc::sdl::parse_key_mapping(settings.values.keymap.value_or("symbolic")).value_or(freetvc::sdl::KeyMapping::Symbolic);
    bool preset_given = false;
    bool skin_given = false;
    bool keymap_given = false;
    std::string language_option = settings.values.language.value_or("");
    std::string renderer_option = settings.values.renderer.value_or("gl");
    std::string picture_option = settings.values.picture.value_or("sharp");
    bool picture_given = false;
    bool colour_switch = settings.values.colour_switch.value_or(true);
    std::string keys_option = settings.values.keys.value_or("hungarian");
    bool keys_given = false;
    bool language_given = false;
    bool remote_monitor = false;
    long monitor_port = freetvc::remote::kMonitorPort;
    std::string moncommands;
    bool mcp_http = false;
    long mcp_port = freetvc::remote::kMcpPort;
    std::string mcp_token;
    bool mcp_allow_monitor = false;
    bool dzrp = false;
    long dzrp_port = freetvc::remote::kDzrpPort;
    auto dzrp_memory = freetvc::remote::DzrpMemory::Zx48k;
    bool gdb = false;
    long gdb_port = freetvc::remote::kGdbPort;
    std::string cas_path;
    std::string cas_dir;
    std::string autostart_path;
    bool autostart_tape = false;
    std::string tape_path;
    std::string tape_record_path;
    std::string cart_path;
    std::string cart_type;
    bool sddual = false;
    bool sddual_gamer = false;
    std::string sddual_rom_path;
    bool sddual_flash_write = false;
    std::array<std::string, 2> sd_paths;
    long cart_bank = 0;
    std::string snapshot_path;
    bool disk_card_wanted = settings.values.disk_card.value_or(false);
    bool disk_card_given = false;
    std::string disk_rom_path;
    std::vector<std::pair<unsigned, std::string>> disks;
    std::optional<unsigned> gamecard_slot;
    if (settings.values.gamecard.value_or(false)) {
        gamecard_slot = settings.values.gamecard_slot.value_or(freetvc::kGameCardSlot);
    }
    bool gamecard_given = false;
    std::string gamecard_rom_path;
    std::optional<unsigned> multichip_slot;
    std::optional<unsigned> twodfx_slot;
    if (settings.values.twodfx.value_or(false)) {
        twodfx_slot = settings.values.twodfx_slot.value_or(freetvc::kCardSlots);
    }
    bool twodfx_given = false;
    if (settings.values.multichip.value_or(false)) {
        multichip_slot = settings.values.multichip_slot.value_or(freetvc::kMultichipSlot);
    }
    bool multichip_given = false;
    std::string multichip_rom_path;
    std::string sid_card = settings.values.sid_card.value_or("off");
    bool sid_card_given = false;
    std::uint32_t sid_clock = freetvc::SidCardConfig{}.clock_hz;
    std::optional<unsigned> nanosd_slot;
    if (settings.values.nanosd.value_or(false)) {
        nanosd_slot = settings.values.nanosd_slot.value_or(freetvc::kNanoSdSlot);
    }
    bool nanosd_given = false;
    std::string nanosd_rom_path;
    std::string nanosd_sd = settings.values.nanosd_sd.value_or("");
    bool nanosd_sd_given = false;
    // No printer and no serial card unless freetvc.ini or the command line brings them
    std::string printer = settings.values.printer.value_or("off");
    std::string printer_raw, printer_text;
    bool printer_given = false;
    std::optional<unsigned> serial_slot;
    if (settings.values.serial_card.value_or(false)) {
        serial_slot = settings.values.serial_slot.value_or(freetvc::kSerialSlot);
    }
    bool serial_given = false;
    std::string serial_line = settings.values.serial_line.value_or("off");
    std::string serial_flow = settings.values.serial_flow.value_or("rts");
    bool serial_line_given = false, serial_flow_given = false;
    std::string serial_send, serial_capture;
    std::optional<unsigned> card256_slot;  // kCardSlots: slot 0, else the first free one
    if (settings.values.card256.value_or(false)) {
        card256_slot = settings.values.card256_slot.value_or(freetvc::kCardSlots);
    }
    bool card256_given = false;
    std::string card256_rom_path;
    std::string card256_usb = settings.values.card256_usb.value_or("");
    std::string card256_flash = settings.values.card256_flash.value_or("");
    bool card256_usb_given = false;
    bool card256_flash_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--card256-usb" || arg == "--card256-flash") && i + 1 < argc) {
            (arg == "--card256-usb" ? card256_usb : card256_flash) = argv[++i];
            (arg == "--card256-usb" ? card256_usb_given : card256_flash_given) = true;
            card256_given = true;
            if (!card256_slot) {
                card256_slot = freetvc::kCardSlots;
            }
        } else if (arg == "--card256") {
            card256_slot = freetvc::kCardSlots;
            card256_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                card256_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--card256-rom" && i + 1 < argc) {
            card256_rom_path = argv[++i];
            card256_given = true;
            if (!card256_slot) {
                card256_slot = freetvc::kCardSlots;
            }
        } else if (arg == "--gamecard") {
            // an optional slot number follows, as in freetvc-headless
            gamecard_slot = freetvc::kGameCardSlot;
            gamecard_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                gamecard_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--gamecard-rom" && i + 1 < argc) {
            gamecard_rom_path = argv[++i];
            gamecard_given = true;
            if (!gamecard_slot) {
                gamecard_slot = freetvc::kGameCardSlot;
            }
        } else if (arg == "--multichip") {
            multichip_slot = freetvc::kMultichipSlot;
            multichip_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                multichip_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--multichip-rom" && i + 1 < argc) {
            multichip_rom_path = argv[++i];
            multichip_given = true;
            if (!multichip_slot) {
                multichip_slot = freetvc::kMultichipSlot;
            }
        } else if (arg == "--sid-card") {
            sid_card = "6581";
            sid_card_given = true;
            if (i + 1 < argc && (std::string_view(argv[i + 1]) == "6581" || std::string_view(argv[i + 1]) == "8580")) {
                sid_card = argv[++i];
            }
        } else if (arg == "--sid-clock" && i + 1 < argc) {
            const unsigned long hz = std::strtoul(argv[++i], nullptr, 10);
            if (hz < 100'000 || hz > 4'000'000) {
                std::fprintf(stderr, "freetvc: --sid-clock: 100000 to 4000000 Hz\n");
                return 1;
            }
            sid_clock = static_cast<std::uint32_t>(hz);
        } else if (arg == "--2dfx") {
            twodfx_slot = 2u;
            twodfx_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                twodfx_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--nanosd") {
            nanosd_slot = freetvc::kNanoSdSlot;
            nanosd_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                nanosd_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--nanosd-rom" && i + 1 < argc) {
            nanosd_rom_path = argv[++i];
            nanosd_given = true;
            if (!nanosd_slot) {
                nanosd_slot = freetvc::kNanoSdSlot;
            }
        } else if (arg == "--nanosd-sd" && i + 1 < argc) {
            nanosd_sd = argv[++i];
            nanosd_sd_given = true;
        } else if (arg == "--printer" && i + 1 < argc) {
            const std::string file = argv[++i];
            printer = "on";
            printer_given = true;
            if (file != "on") {
                (file.size() > 4 && file.compare(file.size() - 4, 4, ".txt") == 0 ? printer_text : printer_raw) = file;
            }
        } else if (arg == "--serial-card") {
            serial_slot = freetvc::kSerialSlot;
            serial_given = true;
            if (i + 1 < argc && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                serial_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if ((arg == "--serial" || arg == "--serial-flow" || arg == "--serial-send" || arg == "--serial-capture") && i + 1 < argc) {
            (arg == "--serial" ? serial_line : arg == "--serial-flow" ? serial_flow : arg == "--serial-send" ? serial_send : serial_capture) = argv[++i];
            serial_line_given |= arg == "--serial";
            serial_flow_given |= arg == "--serial-flow";
            if (!serial_slot) {
                serial_slot = freetvc::kSerialSlot;
                serial_given = true;
            }
        } else if (arg == "--disk-card") {
            disk_card_wanted = disk_card_given = true;
        } else if (arg == "--disk-rom" && i + 1 < argc) {
            disk_rom_path = argv[++i];
            disk_card_wanted = disk_card_given = true;
        } else if (arg == "--disk" && i + 1 < argc) {
            // [A-D:]FILE, as in freetvc-headless
            std::string value = argv[++i];
            std::optional<unsigned> drive;
            if (value.size() > 2 && value[1] == ':' && (drive = freetvc::HostTape::parse_drive(std::string_view(value).substr(0, 2)))) {
                value.erase(0, 2);
            }
            const unsigned d = drive.value_or(static_cast<unsigned>(disks.size()));
            if (d >= freetvc::FloppyDrives::kDrives) {
                std::fprintf(stderr, "freetvc: --disk: there are drives A: to D: only\n");
                return 2;
            }
            disks.emplace_back(d, value);
            disk_card_wanted = disk_card_given = true;
        } else if (arg == "--cas" && i + 1 < argc) {
            cas_path = argv[++i];
        } else if (arg == "--cas-dir" && i + 1 < argc) {
            cas_dir = argv[++i];
        } else if (arg == "--autostart" && i + 1 < argc) {
            autostart_path = argv[++i];
        } else if (arg == "--autostart-tape" && i + 1 < argc) {
            autostart_path = argv[++i];
            autostart_tape = true;
        } else if (arg == "--tape" && i + 1 < argc) {
            tape_path = argv[++i];
        } else if (arg == "--tape-record" && i + 1 < argc) {
            tape_record_path = argv[++i];
        } else if (arg == "--snapshot" && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else if (arg == "--cart" && i + 1 < argc) {
            cart_path = argv[++i];
        } else if (arg == "--cart-type" && i + 1 < argc) {
            cart_type = argv[++i];
        } else if (arg == "--sddual" || arg == "--sddual-gm") {
            sddual = true;
            sddual_gamer = arg == "--sddual-gm";
        } else if (arg == "--sddual-rom" && i + 1 < argc) {
            sddual = true;
            sddual_rom_path = argv[++i];
        } else if (arg == "--sddual-flash-write") {
            sddual_flash_write = true;
        } else if ((arg == "--sd" || arg == "--sd2") && i + 1 < argc) {
            sd_paths[arg == "--sd" ? 0 : 1] = argv[++i];
        } else if (arg == "--cart-bank" && i + 1 < argc) {
            cart_bank = std::strtol(argv[++i], nullptr, 10);
            if (cart_bank < 0 || cart_bank > 63) {
                std::fprintf(stderr, "freetvc: bad bank '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--gdb") {
            gdb = true;
        } else if (arg == "--gdb-port" && i + 1 < argc) {
            gdb_port = std::strtol(argv[++i], nullptr, 10);
            gdb = true;
            if (gdb_port < 0 || gdb_port > 0xFFFF) {
                std::fprintf(stderr, "freetvc: bad port '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--dzrp") {
            dzrp = true;
        } else if (arg == "--dzrp-banks") {
            dzrp_memory = freetvc::remote::DzrpMemory::TvcBanks;
            dzrp = true;
        } else if (arg == "--dzrp-port" && i + 1 < argc) {
            dzrp_port = std::strtol(argv[++i], nullptr, 10);
            dzrp = true;
            if (dzrp_port < 0 || dzrp_port > 0xFFFF) {
                std::fprintf(stderr, "freetvc: bad port '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--mcp-http") {
            mcp_http = true;
        } else if (arg == "--mcp-port" && i + 1 < argc) {
            mcp_port = std::strtol(argv[++i], nullptr, 10);
            mcp_http = true;
            if (mcp_port < 0 || mcp_port > 0xFFFF) {
                std::fprintf(stderr, "freetvc: bad port '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--mcp-token" && i + 1 < argc) {
            mcp_token = argv[++i];
        } else if (arg == "--mcp-allow-monitor") {
            mcp_allow_monitor = true;
        } else if (arg == "--remote-monitor") {
            remote_monitor = true;
        } else if (arg == "--monitor-port" && i + 1 < argc) {
            monitor_port = std::strtol(argv[++i], nullptr, 10);
            remote_monitor = true;
            if (monitor_port < 0 || monitor_port > 0xFFFF) {
                std::fprintf(stderr, "freetvc: bad port '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--moncommands" && i + 1 < argc) {
            moncommands = argv[++i];
        } else if (arg == "--keymap" && i + 1 < argc) {
            const auto mapping = freetvc::sdl::parse_key_mapping(argv[++i]);
            if (!mapping) {
                std::fprintf(stderr, "freetvc: --keymap is symbolic or positional\n");
                return 2;
            }
            key_mapping = *mapping;
            keymap_given = true;
        } else if (arg == "--language" && i + 1 < argc) {
            language_option = argv[++i];
            language_given = true;
        } else if (arg == "--no-audio") {
            audio_wanted = false;
            audio_given = true;
        } else if (arg == "--renderer" && i + 1 < argc && (std::string_view(argv[i + 1]) == "gl" || std::string_view(argv[i + 1]) == "sdl")) {
            renderer_option = argv[++i];
        } else if (arg == "--keys" && i + 1 < argc) {
            keys_option = argv[++i];
            keys_given = true;
        } else if (arg == "--picture" && i + 1 < argc) {
            picture_option = argv[++i];
            picture_given = true;
        } else if (arg == "--full-raster") {
            full_raster = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frame_limit = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "--preset" && i + 1 < argc) {
            preset_name = argv[++i];
            preset_given = true;
        } else if (arg == "--rom-dir" && i + 1 < argc) {
            rom_dir_option = argv[++i];
        } else if (arg == "--skin" && i + 1 < argc) {
            skin_option = argv[++i];
            skin_given = true;
        } else if (arg == "--version") {
            std::printf("freeTVC %s\n", freetvc::kVersion);
            return 0;
        } else {
            std::fprintf(stderr, "usage: freetvc [--preset NAME] [--rom-dir DIR] [--cas FILE] [--cas-dir DIR] [--autostart FILE] "
                                 "[--tape FILE] [--autostart-tape FILE] [--tape-record FILE] "
                                 "[--cart FILE] [--cart-type plain|kilocart|megacart|multicart|sddual] [--cart-bank N] [--snapshot FILE] "
                                 "[--sddual] [--sddual-gm] [--sddual-rom FILE] [--sddual-flash-write] [--sd FILE] [--sd2 FILE] "
                                 "[--disk-card] [--disk-rom 1.1|1.2|FILE] [--disk [A-D:]FILE] [--gamecard [SLOT]] [--gamecard-rom FILE] "
                                 "[--multichip [SLOT]] [--multichip-rom FILE] [--sid-card [6581|8580]] [--sid-clock HZ] [--2dfx [SLOT]] [--nanosd [SLOT]] [--nanosd-rom FILE] [--nanosd-sd DIR] [--printer FILE|on] [--serial-card [SLOT]] [--serial PEER] [--serial-flow rts|none] [--serial-send FILE] [--serial-capture FILE] "
                                 "[--card256 [SLOT]] [--card256-rom FILE] [--card256-usb DIR|mouse|none] [--card256-flash FILE|DIR] "
                                 "[--keymap symbolic|positional] [--language en|hu] "
                                 "[--skin NAME|FILE] [--keys NAME|FILE] [--picture NAME|FILE] [--renderer gl|sdl] [--full-raster] [--no-audio] [--frames N] [--remote-monitor] "
                                 "[--monitor-port N] [--moncommands FILE] [--mcp-http] [--mcp-port N] [--mcp-token T] "
                                 "[--mcp-allow-monitor] [--dzrp] [--dzrp-banks] [--dzrp-port N] [--gdb] [--gdb-port N] "
                                 "[--version]\n");
            return 2;
        }
    }

    // The UI language: --language, freetvc.ini, else the first of the OS's preferred languages that has a translation
    const std::vector<freetvc::sdl::LanguageFile> languages = freetvc::sdl::list_languages(base_dir / "lang");
    const auto use_language = [&](const std::string& code) {
        if (code == "en") {
            freetvc::sdl::use_english();
            return true;
        }
        for (const freetvc::sdl::LanguageFile& file : languages) {
            if (file.code != code) {
                continue;
            }
            std::ifstream in(file.path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            std::vector<std::string> errors;
            freetvc::sdl::Catalog catalog = freetvc::sdl::parse_po(text, errors);
            for (const std::string& line : errors) {
                std::fprintf(stderr, "freetvc: %s %s\n", file.path.string().c_str(), line.c_str());
            }
            freetvc::sdl::use_catalog(std::move(catalog));
            return true;
        }
        return false;
    };
    if (language_option.empty()) {
        std::vector<std::string> preferred;
        int count = 0;
        if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
            for (int n = 0; n < count; ++n) {
                preferred.emplace_back(locales[n]->language);
            }
            SDL_free(locales);
        }
        use_language(freetvc::sdl::pick_language(preferred, languages));
    } else if (!use_language(language_option)) {
        if (language_given) {
            std::fprintf(stderr, "freetvc: --language: no translation '%s' in %s\n", language_option.c_str(), (base_dir / "lang").string().c_str());
            return 2;
        }
        language_option.clear();  // reported once the strip exists
    }
    bool language_chosen = false;  // in the menu; only then it goes to freetvc.ini

    const std::vector<std::string> settings_notices = settings.errors;
    const freetvc::Preset* preset = freetvc::find_preset(preset_name);
    const bool unknown_preset = !preset && !preset_given;
    if (unknown_preset) {
        preset = freetvc::find_preset("64k+-2.2");
    }
    if (!preset) {
        std::fprintf(stderr, "freetvc: unknown preset '%s'\n", preset_name.c_str());
        return 2;
    }
    // ROM search: --rom-dir, FREETVC_ROM_DIR, rom_dir in freetvc.ini, roms/ next to the program, roms/ in the user data folder
    std::string rom_dir_setting = settings.values.rom_dir.value_or("");
    const char* rom_dir_env = std::getenv("FREETVC_ROM_DIR");
    const bool rom_dir_given = !rom_dir_option.empty() || (rom_dir_env && *rom_dir_env);
    std::vector<freetvc::RomPlace> rom_search = freetvc::rom_places(rom_dir_option, rom_dir_setting, base_dir, user_dir);
    std::string error;
    const freetvc::RomPlace* rom_place = nullptr;
    auto config = freetvc::find_preset_roms(*preset, rom_search, error, &rom_place);
    if (!config) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        if (!offer_rom_download(user_dir / "roms")) {
            return 1;
        }
        config = freetvc::find_preset_roms(*preset, rom_search, error, &rom_place);
        if (!config) {
            std::fprintf(stderr, "freetvc: %s\n", error.c_str());
            return 1;
        }
    }
    std::fprintf(stderr, "freetvc: ROMs from %s (%s)\n", rom_place->dir.string().c_str(), rom_place->why.c_str());
    std::string rom_source = rom_place->dir.string() + " (" + rom_place->why + ")";
    std::size_t preset_index = static_cast<std::size_t>(preset - freetvc::kPresets.data());

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "freetvc: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // OpenGL with the picture model, else SDL's renderer
    std::string display_error;
    std::unique_ptr<freetvc::sdl::Display> display = freetvc::sdl::Display::create(
        "freeTVC", 1000, 760, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, renderer_option != "sdl", display_error);
    const ImTextureID texture = display ? display->create_texture(freetvc::Framebuffer::kWidth, freetvc::Framebuffer::kHeight) : ImTextureID_Invalid;
    if (texture == ImTextureID_Invalid) {
        std::fprintf(stderr, "freetvc: window/renderer setup failed: %s\n", display ? SDL_GetError() : display_error.c_str());
        display.reset();
        ImGui::DestroyContext();
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = display->window();
    std::fprintf(stderr, "freetvc: drawing with %s\n", std::string(display->name()).c_str());
    display->set_vsync(true);
    if (fullscreen_at_start) {
        SDL_SetWindowFullscreen(window, true);
    }
    // Bench's Video panel draws a VID bank into this one
    const ImTextureID vram_texture = display->create_texture(freetvc::sdl::BenchView::kVramWidth, freetvc::sdl::BenchView::kVramLines);
    const auto set_title = [&] {
        const std::string title = "freeTVC \xC2\xB7 " + std::string(freetvc::kPresets[preset_index].name);
        SDL_SetWindowTitle(window, title.c_str());
    };
    set_title();


    // Bench's layout lives beside freetvc.ini, so a portable folder keeps both
    const fs::path settings_dir = settings.path.empty() ? user_dir : settings.path.parent_path();
    const std::string layout_file = settings_dir.empty() ? std::string() : (settings_dir / "layout.ini").string();
    io.IniFilename = layout_file.empty() ? nullptr : layout_file.c_str();  // Bench's docking layout (design/ui.md "Bench")
    Notice notice;
    SkinState skins;
    skins.dirs = {user_dir / "skins", base_dir / "skins"};
    skins.font_dir = base_dir / "fonts";
    skins.scale = ui_scale(window);
    if (const auto path = freetvc::sdl::find_skin(skin_option, skins.dirs)) {
        const std::string message = skins.load(*path);
        notice.set(message, !message.empty());
        if (skins.path.empty()) {
            notice.set(skins.apply(), true);
        }
    } else {
        skins.list = freetvc::sdl::list_skins(skins.dirs);
        notice.set(skins.apply(), true);
        notice.show(true, "skin '%s' not found; using the built-in bronze", skin_option.c_str());
    }
    // The picture file, its values handed to the display
    PictureState pictures;
    pictures.dirs = {user_dir / "pictures", base_dir / "pictures"};
    std::string picture_error;
    const auto use_picture = [&](const fs::path& file) {
        picture_error = pictures.load(file);
        if (picture_error.empty() && !display->set_picture(pictures.picture, picture_error)) {
            std::fprintf(stderr, "freetvc: %s\n", picture_error.c_str());
        } else if (!picture_error.empty()) {
            notice.set(picture_error, true);
        }
    };
    if (const auto path = pictures.find(picture_option)) {
        use_picture(*path);
    } else {
        pictures.list = freetvc::sdl::list_pictures(pictures.dirs);
        notice.show(true, "picture '%s' not found; sharp pixels", picture_option.c_str());
    }
    for (const std::string& line : settings_notices) {
        notice.set(line, true);
    }
    if (unknown_preset) {
        notice.show(true, "freetvc.ini: unknown preset '%s'; using 64k+-2.2", preset_name.c_str());
    }
    if (settings.values.language && language_option.empty()) {
        notice.show(true, "freetvc.ini: no translation '%s'; using English", settings.values.language->c_str());
    }
    // What freetvc.ini gets on exit: a skin from a skins folder by name, any other by path
    const auto skin_setting = [&]() -> std::string {
        if (skins.path.empty()) {
            return "bronze";
        }
        for (const fs::path& dir : skins.dirs) {
            if (skins.path.parent_path().lexically_normal() == dir.lexically_normal()) {
                return skins.path.stem().string();
            }
        }
        return skins.path.string();
    };
    const std::string skin_at_start = skin_setting();
    const std::size_t preset_at_start = static_cast<std::size_t>(preset - freetvc::kPresets.data());
    const freetvc::sdl::KeyMapping mapping_at_start = key_mapping;

    config->colour = colour_switch;
    freetvc::Machine machine(*config);
    freetvc::sdl::HostInput input(key_mapping);
    // The host key layout; the user data folder's keymaps/ first, where the editor keeps its copies
    const std::vector<fs::path> keymap_dirs{user_dir / "keymaps", base_dir / "keymaps"};
    std::vector<freetvc::sdl::KeymapEntry> keymaps = freetvc::sdl::list_keymaps(keymap_dirs);
    fs::path keymap_path;
    const auto use_keymap = [&](const fs::path& file) {
        std::string why;
        if (auto loaded = freetvc::sdl::load_keymap_file(file, why)) {
            input.set_keymap(std::move(*loaded));
            keymap_path = file;
        } else {
            notice.set(why, true);
        }
        keymaps = freetvc::sdl::list_keymaps(keymap_dirs);
    };
    const auto find_keymap = [&](const std::string& name_or_path) -> std::optional<fs::path> {
        for (const auto& entry : freetvc::sdl::list_keymaps(keymap_dirs)) {
            if (entry.name == name_or_path) {
                return entry.path;
            }
        }
        std::error_code ec;
        return fs::is_regular_file(name_or_path, ec) ? std::optional<fs::path>(name_or_path) : std::nullopt;
    };
    if (const auto path = find_keymap(keys_option)) {
        use_keymap(*path);
    } else {
        notice.show(true, "key layout '%s' not found; the built-in hungarian", keys_option.c_str());
    }
    freetvc::sdl::KeyboardEditor keyboard_editor;
    // The editor's changes go into the user data folder's copy of the layout, which then shadows the shipped file
    const auto save_keymap = [&] {
        const fs::path copy = user_dir / "keymaps" / (input.keymap().name + ".keys");
        std::error_code ec;
        fs::create_directories(copy.parent_path(), ec);
        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
        out << "; " << input.keymap().name << " as changed in freeTVC's keyboard editor\n" << freetvc::sdl::keymap_text(input.keymap());
        out.close();
        if (!out) {
            notice.show(true, "cannot write '%s'", copy.string().c_str());
            return;
        }
        keymap_path = copy;
        keymaps = freetvc::sdl::list_keymaps(keymap_dirs);
    };
    freetvc::HostTape tape(machine);
    tape.set_rom_places(rom_search);
    tape.set_prompt_dir(user_dir / "prompt");
    // --disk-rom, else disk_rom in freetvc.ini; 1.1 is the search's hbf.rom, which the card finds itself
    const bool disk_rom_given = !disk_rom_path.empty();
    if (!disk_rom_given) {
        disk_rom_path = settings.values.disk_rom.value_or("1.1");
    }
    if (disk_rom_path != "1.1") {
        auto rom = freetvc::disk_card_rom(disk_rom_path, rom_search, error);
        if (!rom && disk_rom_given) {
            std::fprintf(stderr, "freetvc: --disk-rom: %s\n", error.c_str());
            return 1;
        }
        if (rom) {
            tape.set_disk_rom(std::move(*rom));
        } else {
            std::fprintf(stderr, "freetvc: disk_rom: %s\n", error.c_str());
        }
    }
    // The disk card: from freetvc.ini or the command line; a card ROM that cannot be found is a notice, not an exit
    std::string disk_card_problem;
    if (disk_card_wanted && !tape.disk_card(true, disk_card_problem)) {
        if (disk_card_given) {
            std::fprintf(stderr, "freetvc: %s\n", disk_card_problem.c_str());
            return 1;
        }
        notice.show(true, "disk card left out: %s", disk_card_problem.substr(0, disk_card_problem.find("; searched")).c_str());
    }
    for (const auto& [drive, path] : disks) {
        if (!tape.disk_insert(drive, path, error)) {
            std::fprintf(stderr, "freetvc: --disk: %s\n", error.c_str());
            return 1;
        }
    }
    // A disk autostart from the command line brings the card for this run only
    if (!autostart_path.empty() && freetvc::HostTape::is_disk_file(autostart_path)) {
        disk_card_given = true;
    }
    const bool disk_card_at_start = tape.disk_card_on();
    // The GameCard: from freetvc.ini or the command line
    if (!gamecard_rom_path.empty()) {
        auto rom = freetvc::read_file(gamecard_rom_path, 0x10000, error);
        if (!rom) {
            std::fprintf(stderr, "freetvc: --gamecard-rom: %s\n", error.c_str());
            return 1;
        }
        tape.set_gamecard_rom(std::move(*rom));
    }
    if (gamecard_slot && !tape.gamecard(true, *gamecard_slot, error)) {
        if (gamecard_given) {
            std::fprintf(stderr, "freetvc: --gamecard: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "GameCard left out: %s", error.c_str());
    }
    // The Multichip sound card: from freetvc.ini or the command line
    if (!multichip_rom_path.empty()) {
        auto rom = freetvc::read_file(multichip_rom_path, freetvc::MultichipCard::kRomSize, error);
        if (!rom) {
            std::fprintf(stderr, "freetvc: --multichip-rom: %s\n", error.c_str());
            return 1;
        }
        tape.set_multichip_rom(std::move(*rom));
    }
    if (multichip_slot && !tape.multichip(true, *multichip_slot, error)) {
        if (multichip_given) {
            std::fprintf(stderr, "freetvc: --multichip: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "Multichip sound card left out: %s", error.c_str());
    }
    // The SID card: from freetvc.ini or the command line
    tape.set_sid_clock(sid_clock);
    if (sid_card != "off" && !tape.sid_card(sid_card, error)) {
        std::fprintf(stderr, "freetvc: --sid-card: %s\n", error.c_str());
        return 1;
    }
    // The printer and the serial card: from freetvc.ini or the command line
    if (printer != "off") {
        if (!printer_given && printer != "on") {
            (printer.size() > 4 && printer.compare(printer.size() - 4, 4, ".txt") == 0 ? printer_text : printer_raw) = printer;
        }
        if (!tape.printer_plug(printer_raw, printer_text, error)) {
            if (printer_given) {
                std::fprintf(stderr, "freetvc: --printer: %s\n", error.c_str());
                return 1;
            }
            notice.show(true, "printer left out: %s", error.c_str());
        }
    }
    if (serial_slot && !tape.serial_card(true, *serial_slot, error)) {
        if (serial_given) {
            std::fprintf(stderr, "freetvc: --serial-card: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "serial card left out: %s", error.c_str());
    }
    if (serial_flow != "rts" && serial_flow != "none") {
        std::fprintf(stderr, "freetvc: --serial-flow: rts or none\n");
        return 1;
    }
    tape.serial_flow(serial_flow == "rts");
    if (serial_line == "off" && (!serial_send.empty() || !serial_capture.empty())) {
        serial_line = "on";
    }
    if (!tape.serial_peer(serial_line, error)) {
        if (serial_line_given) {
            std::fprintf(stderr, "freetvc: --serial: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "serial cable left unplugged: %s", error.c_str());
    }
    if ((!serial_send.empty() && !tape.serial_queue_file(serial_send, error)) || (!serial_capture.empty() && !tape.serial_capture(serial_capture, error))) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        return 1;
    }
    std::string serial_capture_name = serial_capture.empty() ? std::string() : fs::path(serial_capture).filename().string();
    std::string printer_name = tape.printer_setting();
    // The NanoSD: from freetvc.ini or the command line; its EEPROM settings too. --nanosd-sd alone brings the card,
    // unless a snapshot brings the machine
    if (nanosd_sd_given && !nanosd_slot && snapshot_path.empty()) {
        nanosd_slot = freetvc::kNanoSdSlot;
        nanosd_given = true;
    }
    if (!nanosd_rom_path.empty()) {
        auto rom = freetvc::read_file(nanosd_rom_path, freetvc::NanoSd::kRomSize, error);
        if (!rom) {
            std::fprintf(stderr, "freetvc: --nanosd-rom: %s\n", error.c_str());
            return 1;
        }
        tape.set_nanosd_rom(std::move(*rom));
    }
    if (settings.values.nanosd_menu || settings.values.nanosd_sort) {
        freetvc::NanoSdMedia& eeprom = tape.nanosd_media();
        eeprom.eeprom_valid = true;
        eeprom.menu = settings.values.nanosd_menu.value_or(true) ? 1 : 0;
        eeprom.sort = settings.values.nanosd_sort.value_or(0);
    }
    if (!nanosd_sd.empty() && !tape.nanosd_sd(nanosd_sd, error)) {
        if (nanosd_sd_given) {
            std::fprintf(stderr, "freetvc: --nanosd-sd: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "NanoSD's SD card left out: %s", error.c_str());
    }
    const auto nanosd_sd_setting = [&tape] {
        const fs::path dir = tape.nanosd_sd_dir();
        return dir.empty() ? std::string("none") : dir.string();
    };
    const std::string nanosd_sd_at_start = nanosd_sd_setting();
    if (nanosd_slot && !tape.nanosd(true, *nanosd_slot, error)) {
        if (nanosd_given) {
            std::fprintf(stderr, "freetvc: --nanosd: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "NanoSD left out: %s", error.c_str());
    }
    if (twodfx_slot && !tape.twodfx(true, *twodfx_slot, error)) {
        if (twodfx_given) {
            std::fprintf(stderr, "freetvc: --2dfx: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "2dfx card left out: %s", error.c_str());
    }
    // The TVC256k++: from freetvc.ini or the command line
    if (!card256_rom_path.empty()) {
        auto rom = freetvc::read_file(card256_rom_path, freetvc::Card256::kRomSize, error);
        if (!rom) {
            std::fprintf(stderr, "freetvc: --card256-rom: %s\n", error.c_str());
            return 1;
        }
        tape.set_card256_rom(std::move(*rom));
    }
    // The flash drive before the card goes in, so the ROM search is not asked for one
    if (!card256_flash.empty() && !tape.card256_flash(card256_flash, error)) {
        if (card256_flash_given) {
            std::fprintf(stderr, "freetvc: --card256-flash: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "TVC256k++ flash drive left out: %s", error.c_str());
    }
    if (!card256_usb.empty() && !tape.card256_usb(card256_usb, error)) {
        if (card256_usb_given) {
            std::fprintf(stderr, "freetvc: --card256-usb: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "TVC256k++ pendrive left out: %s", error.c_str());
    }
    // What freetvc.ini's card256_usb holds for the port as it is
    const auto card256_usb_setting = [&tape] {
        const freetvc::Card256Usb usb = tape.card256_host().usb();
        return usb == freetvc::Card256Usb::Mouse      ? std::string("mouse")
               : usb == freetvc::Card256Usb::Pendrive ? tape.card256_host().pendrive_dir().string()
                                                      : std::string("none");
    };
    const std::string card256_usb_at_start = card256_usb_setting();
    const std::string card256_flash_at_start = card256_flash;
    if (card256_slot && !tape.card256(true, *card256_slot, error)) {
        if (card256_given) {
            std::fprintf(stderr, "freetvc: --card256: %s\n", error.c_str());
            return 1;
        }
        notice.show(true, "TVC256k++ left out: %s", error.c_str());
    }
    for (std::string& line : tape.take_messages()) {
        notice.set(std::move(line), true);
    }
    const int gamecard_at_start = tape.gamecard_slot();
    const int multichip_at_start = tape.multichip_slot();
    const int twodfx_at_start = tape.twodfx_slot();
    const std::string sid_card_at_start = tape.sid_card_model();
    const int card256_at_start = tape.card256_slot();
    const int nanosd_at_start = tape.nanosd_slot();
    const std::string printer_at_start = tape.printer_setting();
    const int serial_at_start = tape.serial_slot();
    const std::string serial_line_at_start = tape.serial_peer_name();
    const bool serial_flow_at_start = tape.serial_rts_flow();
    tape.set_sddual_flash_writable(sddual_flash_write);
    if (!cart_path.empty() && !tape.cart_insert(cart_path, cart_type, static_cast<unsigned>(cart_bank), error)) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        return 1;
    }
    // The SD Dual: its firmware from the ROM search or --sddual-rom; --sd alone brings it unless a cartridge or a
    // snapshot brings the machine
    if ((!sd_paths[0].empty() || !sd_paths[1].empty()) && cart_path.empty() && snapshot_path.empty()) {
        sddual = true;
    }
    if (sddual) {
        if (!cart_path.empty()) {
            std::fprintf(stderr, "freetvc: --sddual: the cartridge port holds %s\n", cart_path.c_str());
            return 1;
        }
        if (!sddual_rom_path.empty()) {
            auto rom = freetvc::read_file(sddual_rom_path, freetvc::SdDual::kFlashSize, error);
            if (!rom) {
                std::fprintf(stderr, "freetvc: --sddual-rom: %s\n", error.c_str());
                return 1;
            }
            tape.set_sddual_rom(std::move(*rom));
        }
        if (!tape.sddual(true, sddual_gamer, error)) {
            std::fprintf(stderr, "freetvc: --sddual: %s\n", error.c_str());
            return 1;
        }
    }
    for (unsigned s = 0; s < sd_paths.size(); ++s) {
        if (!sd_paths[s].empty() && !tape.sd_insert(s, sd_paths[s], error)) {
            std::fprintf(stderr, "freetvc: %s: %s\n", s == 0 ? "--sd" : "--sd2", error.c_str());
            return 1;
        }
    }
    if (!cas_dir.empty() && !tape.set_folder(cas_dir, error)) {
        std::fprintf(stderr, "freetvc: --cas-dir: %s\n", error.c_str());
        return 1;
    }
    if (!cas_path.empty() && !tape.attach(cas_path, error)) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        return 1;
    }
    if ((!tape_path.empty() && !tape.tape_insert(tape_path, error)) ||
        (!tape_record_path.empty() && !tape.tape_record(tape_record_path, error))) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        return 1;
    }
    if (!tape_path.empty()) {
        tape.tape_play();
    }
    // After a snapshot load the board and ROMs may be another preset's: the title and the menu follow
    const auto sync_preset = [&] {
        const freetvc::MachineConfig& now = machine.config();
        for (std::size_t i = 0; i < freetvc::kPresets.size(); ++i) {
            std::string ignored;
            if (freetvc::kPresets[i].model != now.model) {
                continue;
            }
            const auto roms = freetvc::find_preset_roms(freetvc::kPresets[i], rom_search, ignored);
            if (roms && roms->roms.sys == now.roms.sys && roms->roms.ext == now.roms.ext) {
                preset_index = i;
                break;
            }
        }
        set_title();
    };
    if (!snapshot_path.empty()) {
        if (!tape.snapshot_load(snapshot_path, error)) {
            std::fprintf(stderr, "freetvc: %s\n", error.c_str());
            return 1;
        }
        sync_preset();
    }
    if (!autostart_path.empty() && !(autostart_tape ? tape.tape_autostart(autostart_path, error) : tape.autostart(autostart_path, error))) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
        return 1;
    }

    // The text monitor (design/debug-and-remote.md "Text monitor") always exists: Pause and Bench go through it
    auto debugger = std::make_unique<freetvc::debug::Debugger>(machine);
    auto monitor = std::make_unique<freetvc::remote::Monitor>(*debugger);
    auto hub = std::make_unique<freetvc::remote::MonitorHub>(*monitor);
    std::unique_ptr<freetvc::remote::McpServer> mcp;
    std::unique_ptr<freetvc::remote::McpHost> mcp_host;
    std::unique_ptr<freetvc::remote::DzrpServer> dzrp_server;
    std::unique_ptr<freetvc::remote::DzrpHost> dzrp_host;
    std::unique_ptr<freetvc::remote::GdbServer> gdb_server;
    std::unique_ptr<freetvc::remote::GdbHost> gdb_host;
    // The servers: the command line, else freetvc.ini; the settings sheet starts and stops them while running
    struct ServerState {
        bool on = false;
        long port = 0;
        bool given = false;
        std::string error;
    };
    std::array<ServerState, 4> servers{{{remote_monitor, monitor_port, remote_monitor, {}},
                                        {mcp_http, mcp_port, mcp_http, {}},
                                        {dzrp, dzrp_port, dzrp, {}},
                                        {gdb, gdb_port, gdb, {}}}};
    const std::array<std::optional<int>, 4> server_settings{settings.values.monitor, settings.values.mcp, settings.values.dzrp, settings.values.gdb};
    for (std::size_t i = 0; i < servers.size(); ++i) {
        if (!servers[i].given && server_settings[i]) {
            servers[i].on = *server_settings[i] != freetvc::kServerOff;
            if (*server_settings[i] > 0) {
                servers[i].port = *server_settings[i];
            }
        }
    }
    const std::array<long, 4> default_ports{freetvc::remote::kMonitorPort, freetvc::remote::kMcpPort, freetvc::remote::kDzrpPort,
                                            freetvc::remote::kGdbPort};
    if (!mcp_allow_monitor) {
        mcp_allow_monitor = settings.values.mcp_allow_monitor.value_or(false);
    }
    if (dzrp_memory == freetvc::remote::DzrpMemory::Zx48k && settings.values.dzrp_banks.value_or(false)) {
        dzrp_memory = freetvc::remote::DzrpMemory::TvcBanks;
    }
    const std::string mcp_token_now = mcp_token.empty() ? freetvc::remote::McpHost::make_token() : mcp_token;
    // Starts or stops server `which` (freetvc::sdl::Server order); a failed start keeps its reason for the sheet
    const auto set_server = [&](std::size_t which, bool on) -> bool {
        ServerState& server = servers[which];
        std::string why;
        switch (which) {
        case freetvc::sdl::kMonitorServer:
            hub->stop_listening();
            if (on && !hub->listen(static_cast<std::uint16_t>(server.port), why)) {
                hub->stop_listening();
            } else if (on) {
                std::fprintf(stderr, "freetvc: text monitor on 127.0.0.1:%u\n", static_cast<unsigned>(hub->port()));
            }
            break;
        case freetvc::sdl::kMcpServer:
            mcp_host.reset();
            mcp.reset();
            if (on) {
                freetvc::remote::McpOptions options;
                options.machine_name = std::string(freetvc::kPresets[preset_index].name);
                options.allow_monitor_command = mcp_allow_monitor;
                mcp = std::make_unique<freetvc::remote::McpServer>(*monitor, options);
                mcp_host = std::make_unique<freetvc::remote::McpHost>(*mcp);
                if (!mcp_host->listen_http(static_cast<std::uint16_t>(server.port), mcp_token_now, why)) {
                    mcp_host.reset();
                    mcp.reset();
                } else {
                    std::fprintf(stderr, "freetvc: MCP on http://127.0.0.1:%u/mcp (Authorization: Bearer %s)\n",
                                 static_cast<unsigned>(mcp_host->http_port()), mcp_token_now.c_str());
                }
            }
            break;
        case freetvc::sdl::kDzrpServer:
            dzrp_host.reset();
            dzrp_server.reset();
            if (on) {
                dzrp_server = std::make_unique<freetvc::remote::DzrpServer>(*monitor, dzrp_memory);
                dzrp_host = std::make_unique<freetvc::remote::DzrpHost>(*dzrp_server);
                if (!dzrp_host->listen(static_cast<std::uint16_t>(server.port), why)) {
                    dzrp_host.reset();
                    dzrp_server.reset();
                } else {
                    std::fprintf(stderr, "freetvc: DZRP on 127.0.0.1:%u\n", static_cast<unsigned>(dzrp_host->port()));
                }
            }
            break;
        default:
            gdb_host.reset();
            gdb_server.reset();
            if (on) {
                gdb_server = std::make_unique<freetvc::remote::GdbServer>(*monitor);
                gdb_host = std::make_unique<freetvc::remote::GdbHost>(*gdb_server);
                if (!gdb_host->listen(static_cast<std::uint16_t>(server.port), why)) {
                    gdb_host.reset();
                    gdb_server.reset();
                } else {
                    std::fprintf(stderr, "freetvc: GDB stub on 127.0.0.1:%u\n", static_cast<unsigned>(gdb_host->port()));
                }
            }
            break;
        }
        server.on = on;
        server.error = why;
        if (!why.empty()) {
            std::fprintf(stderr, "freetvc: %s\n", why.c_str());
        }
        return why.empty();
    };
    const auto server_port = [&](std::size_t which) -> int {
        switch (which) {
        case freetvc::sdl::kMonitorServer:
            return hub->listening() ? hub->port() : 0;
        case freetvc::sdl::kMcpServer:
            return mcp_host ? mcp_host->http_port() : 0;
        case freetvc::sdl::kDzrpServer:
            return dzrp_host ? dzrp_host->port() : 0;
        default:
            return gdb_host ? gdb_host->port() : 0;
        }
    };
    {
        // A server the command line asked for must start; one from freetvc.ini reports its problem in the sheet
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (servers[i].on && !set_server(i, true) && servers[i].given) {
                return 1;
            }
        }
        if (!moncommands.empty()) {
            std::ifstream in(moncommands);
            if (!in) {
                std::fprintf(stderr, "freetvc: cannot read '%s'\n", moncommands.c_str());
                return 1;
            }
            std::vector<std::string> lines;
            for (std::string line; std::getline(in, line);) {
                lines.push_back(line);
            }
            hub->add_script(std::move(lines));
        }
    }
    monitor->set_media(&tape);
    // The machine picture (about/tvc64kplus.png) covers the boot at start, at power on and when a card changes: at least
    // kSplashNs from the first frame that shows it, until the machine draws something (a 64k boots ~5 s black), then it fades
    constexpr std::uint64_t kSplashNs = 2'500'000'000;
    constexpr std::uint64_t kSplashMaxNs = 10'000'000'000;
    constexpr std::uint64_t kSplashFadeNs = 500'000'000;
    ImTextureID splash = ImTextureID_Invalid;
    ImVec2 splash_size{};
    if (SDL_Surface* png = SDL_LoadPNG((base_dir / "about" / "tvc64kplus.png").string().c_str())) {
        if (SDL_Surface* argb = SDL_ConvertSurface(png, SDL_PIXELFORMAT_ARGB8888)) {
            splash = display->create_texture(argb->w, argb->h);
            display->update_texture(splash, argb->pixels, argb->pitch);
            splash_size = {static_cast<float>(argb->w), static_cast<float>(argb->h)};
            SDL_DestroySurface(argb);
        }
        SDL_DestroySurface(png);
    }
    bool splash_on = splash != ImTextureID_Invalid;
    std::uint64_t splash_from_ns = 0;  // the first frame that showed it; 0 = not shown yet
    std::uint64_t splash_fade_ns = 0;  // when the fade began; 0 = not yet
    bool splash_drawn = false;         // the machine has put something other than black on the screen
    const auto show_machine = [&] {
        splash_on = splash != ImTextureID_Invalid;
        splash_from_ns = 0;
        splash_fade_ns = 0;
        splash_drawn = false;
    };
    const auto held = [&] { return monitor->active(); };
    // Bench's Console is a local hub client; the GUI's own monitor commands are echoed there too (design/ui.md "Bench", 7d)
    const int console_client = hub->attach_local();
    freetvc::sdl::BenchView bench_view;
    bench_view.install_settings();
    bench_view.set_vram_texture(display.get(), vram_texture);
    if (mcp_host) {
        bench_view.set_mcp(mcp.get(), "http://127.0.0.1:" + std::to_string(mcp_host->http_port()) + "/mcp");
    }
    // The machine's state for freetvc.ini, except a command-line value the menus left alone; written when it changes
    // A slot card's key: no, yes for its own slot, else the slot number
    const auto card_setting = [](int slot, unsigned own) {
        return slot < 0 ? std::string("no") : slot == static_cast<int>(own) ? std::string("yes") : std::to_string(slot);
    };
    const auto collect_settings = [&] {
        freetvc::SettingValues values;
        if (!preset_given || preset_index != preset_at_start) {
            values.emplace_back("preset", std::string(freetvc::kPresets[preset_index].name));
        }
        if (!skin_given || skin_setting() != skin_at_start) {
            values.emplace_back("skin", skin_setting());
        }
        if (language_chosen) {
            values.emplace_back("language", freetvc::sdl::current_language());
        }
        if (!keymap_given || input.mapping() != mapping_at_start) {
            values.emplace_back("keymap", input.mapping() == freetvc::sdl::KeyMapping::Symbolic ? "symbolic" : "positional");
        }
        if ((!disk_card_given && (settings.values.disk_card.has_value() || tape.disk_card_on())) || tape.disk_card_on() != disk_card_at_start) {
            values.emplace_back("disk_card", tape.disk_card_on() ? "yes" : "no");
        }
        const int gamecard_now = tape.gamecard_slot();
        if ((!gamecard_given && (settings.values.gamecard.has_value() || gamecard_now >= 0)) || gamecard_now != gamecard_at_start) {
            values.emplace_back("gamecard", card_setting(gamecard_now, freetvc::kGameCardSlot));
        }
        const int multichip_now = tape.multichip_slot();
        if ((!multichip_given && (settings.values.multichip.has_value() || multichip_now >= 0)) || multichip_now != multichip_at_start) {
            values.emplace_back("multichip", card_setting(multichip_now, freetvc::kMultichipSlot));
        }
        const int twodfx_now = tape.twodfx_slot();
        if ((!twodfx_given && (settings.values.twodfx.has_value() || twodfx_now >= 0)) || twodfx_now != twodfx_at_start) {
            values.emplace_back("twodfx", card_setting(twodfx_now, 2));
        }
        const std::string sid_now = tape.sid_card_model();
        if ((!sid_card_given && (settings.values.sid_card.has_value() || sid_now != "off")) || sid_now != sid_card_at_start) {
            values.emplace_back("sid_card", sid_now);
        }
        const int nanosd_now = tape.nanosd_slot();
        if ((!nanosd_given && (settings.values.nanosd.has_value() || nanosd_now >= 0)) || nanosd_now != nanosd_at_start) {
            values.emplace_back("nanosd", card_setting(nanosd_now, freetvc::kNanoSdSlot));
        }
        const std::string printer_now = tape.printer_setting();
        if ((!printer_given && settings.values.printer.value_or("off") != printer_now) || printer_now != printer_at_start) {
            values.emplace_back("printer", printer_now);
        }
        const int serial_now = tape.serial_slot();
        if ((!serial_given && (settings.values.serial_card.has_value() || serial_now >= 0)) || serial_now != serial_at_start) {
            values.emplace_back("serial_card", card_setting(serial_now, freetvc::kSerialSlot));
        }
        // A cable to a host peer only makes sense with its files, which are not saved
        const std::string line_now = tape.serial_peer_name() == "on" ? "off" : tape.serial_peer_name();
        if ((!serial_line_given && settings.values.serial_line.value_or("off") != line_now) || tape.serial_peer_name() != serial_line_at_start) {
            values.emplace_back("serial_line", line_now);
        }
        if ((!serial_flow_given && settings.values.serial_flow.has_value()) || tape.serial_rts_flow() != serial_flow_at_start) {
            values.emplace_back("serial_flow", tape.serial_rts_flow() ? "rts" : "none");
        }
        const std::string sd_now = nanosd_sd_setting();
        if ((!nanosd_sd_given && settings.values.nanosd_sd.value_or("none") != sd_now) || sd_now != nanosd_sd_at_start) {
            values.emplace_back("nanosd_sd", sd_now);
        }
        if (const freetvc::NanoSdMedia& eeprom = tape.nanosd_media(); eeprom.eeprom_valid) {
            values.emplace_back("nanosd_menu", eeprom.menu ? "yes" : "no");
            values.emplace_back("nanosd_sort", eeprom.sort == 1 ? "name" : eeprom.sort == 2 ? "size" : "none");
        }
        const int card256_now = tape.card256_slot();
        if ((!card256_given && (settings.values.card256.has_value() || card256_now >= 0)) || card256_now != card256_at_start) {
            values.emplace_back("card256", card_setting(card256_now, freetvc::kCard256Slot));
        }
        const std::string usb_now = card256_usb_setting();
        if ((!card256_usb_given && settings.values.card256_usb.value_or("none") != usb_now) || usb_now != card256_usb_at_start) {
            values.emplace_back("card256_usb", usb_now);
        }
        if (!card256_flash.empty() && ((!card256_flash_given && settings.values.card256_flash != card256_flash) || card256_flash != card256_flash_at_start)) {
            values.emplace_back("card256_flash", card256_flash);
        }
        return values;
    };
    // A missing file is written at the first check, as it was on exit before the settings window
    std::error_code settings_ec;
    freetvc::SettingValues saved_settings =
        !settings.path.empty() && fs::exists(settings.path, settings_ec) ? collect_settings() : freetvc::SettingValues{};
    const auto save_settings = [&](const freetvc::SettingValues& values) {
        std::string why;
        if (!settings.path.empty() && !values.empty() && !freetvc::save_settings(settings.path, values, why)) {
            std::fprintf(stderr, "freetvc: %s\n", why.c_str());
        }
    };
    std::uint64_t settings_checked_ns = 0;
    freetvc::sdl::SettingsView settings_view;
    freetvc::sdl::AboutView about_view;
    const std::string settings_file = settings.path.string();
    // The disk card's EPROM by choice: a card that is in takes it at once and the machine powers on
    const auto use_disk_rom = [&](const std::string& choice) {
        std::string why;
        auto rom = freetvc::disk_card_rom(choice, rom_search, why);
        if (!rom) {
            notice.set(why, true);
            return;
        }
        tape.set_disk_rom(std::move(*rom));
        disk_rom_path = choice;
        if (tape.disk_card_on() && tape.disk_card(false, why) && tape.disk_card(true, why)) {
            debugger->cancel_step();
            monitor->machine_changed();
            show_machine();
            notice.show_text(false, N_("the disk card has its new EPROM; the machine powered on"));
        } else if (!why.empty()) {
            notice.set(why, true);
        }
        save_settings({{"disk_rom", choice}});
    };
    // A new rom_dir (empty: the default places); the machine powers on only when the preset's ROMs differ
    int rom_reload = -1;
    // Video recording and screenshots: the folders from freetvc.ini, else the OS's videos folder and desktop
    std::string video_dir_setting = settings.values.video_dir.value_or("");
    std::string screenshot_dir_setting = settings.values.screenshot_dir.value_or("");
    const auto video_dir = [&] {
        return video_dir_setting.empty() ? user_folder(SDL_FOLDER_VIDEOS, user_dir / "videos") : fs::path(video_dir_setting);
    };
    const auto screenshot_dir = [&] {
        return screenshot_dir_setting.empty() ? user_folder(SDL_FOLDER_DESKTOP, user_dir / "screenshots") : fs::path(screenshot_dir_setting);
    };
    // The running program's name for file names, else the machine's
    const auto media_name = [&]() -> std::string {
        const std::string cassette(tape.media().cassette().attached_name());
        return cassette.empty() ? std::string(freetvc::kPresets[preset_index].name) : cassette;
    };
    freetvc::VideoRecorder recorder;
    const auto stop_recording = [&] {
        machine.set_frame_sink(nullptr);
        std::vector<float> rest(machine.audio_available());
        machine.read_audio(rest);
        recorder.audio(rest);
        std::string why;
        if (recorder.stop(why)) {
            notice.show(false, "saved %s (%.0f s)", utf8(recorder.file()).c_str(), recorder.seconds());
        } else {
            notice.show(true, "recording failed: %s", why.c_str());
        }
    };
    const auto use_rom_dir = [&](const std::string& dir) {
        rom_dir_setting = dir;
        rom_search = freetvc::rom_places(rom_dir_option, rom_dir_setting, base_dir, user_dir);
        tape.set_rom_places(rom_search);
        save_settings({{"rom_dir", dir}});
        std::string why;
        const freetvc::RomPlace* place = nullptr;
        if (auto found = freetvc::find_preset_roms(freetvc::kPresets[preset_index], rom_search, why, &place)) {
            rom_source = place->dir.string() + " (" + place->why + ")";
            if (found->roms.sys != machine.config().roms.sys || found->roms.ext != machine.config().roms.ext) {
                rom_reload = static_cast<int>(preset_index);
            }
        } else {
            std::fprintf(stderr, "freetvc: %s\n", why.c_str());
            notice.show(true, "%s (the places searched are on stderr)", why.substr(0, why.find("; searched")).c_str());
        }
    };
    const auto run_command = [&](const std::string& line) {
        bench_view.console_command(monitor->prompt(), line);
        const std::string out = monitor->execute(line);
        bench_view.console_output(out);
        bench_view.command_result(line, out);
        if (out.starts_with("error:")) {
            notice.set(line + ": " + out.substr(0, out.find('\n')), true);
        }
    };
    const auto toggle_hold = [&] {
        if (held()) {
            run_command("x");
        } else {
            bench_view.console_output(monitor->enter("pause"));
        }
    };
    bool was_held = false;
    bool bench = false;
    bool keys_to_tvc = true;
    const auto set_bench = [&](bool on) {
        bench = on;
        if (bench) {
            bench_view.focus_screen();
        }
    };

    // Program media (design/media.md): autostart runs unpaced with the screen blanked until the program starts
    FileChoice file_choice;
    std::vector<std::string> multicart_files;  // chosen for a new MultiCart image, waiting for its file name
    std::vector<std::string> sd_new_files;     // chosen for a new SD card image, waiting for its file name
    bool warping = false;
    const freetvc::Framebuffer blank;
    const auto insert_cart = [&](const std::string& path) {
        std::string why;
        if (!tape.cart_insert(path, "", 0, why)) {
            notice.set(why, true);
            return;
        }
        debugger->cancel_step();
        monitor->machine_changed();
        show_machine();
        input.machine_replaced();
        notice.show(false, "%s; powered on", tape.cart_describe().c_str());
    };
    // design/cards.md: a disk going in brings the card, which powers the machine on
    const auto insert_disk = [&](unsigned drive, const std::string& path) {
        const bool card_was_on = tape.disk_card_on();
        std::string why;
        if (!tape.disk_insert(drive, path, why)) {
            notice.set(why, true);
            return;
        }
        if (!card_was_on) {
            debugger->cancel_step();
            monitor->machine_changed();
            show_machine();
            input.machine_replaced();
        }
        const std::string name = tape.disk_name(drive);
        if (card_was_on) {
            notice.set(std::string(1, static_cast<char>('A' + drive)) + ": " + name, false);
        } else {
            notice.show(false, "%c: %s; disk card in, powered on", static_cast<char>('A' + drive), name.c_str());
        }
    };
    // design/cards.md "SD Dual": a card going in brings the cartridge, which powers the machine on
    const auto insert_sd = [&](unsigned socket, const std::string& path) {
        std::string why;
        const bool cart_was_in = tape.sddual_on();
        if (!cart_was_in && !tape.sddual(true, false, why)) {
            notice.show(true, "SD Dual: %s", why.c_str());
            return;
        }
        if (!cart_was_in) {
            debugger->cancel_step();
            monitor->machine_changed();
            show_machine();
            input.machine_replaced();
        }
        if (!tape.sd_insert(socket, path, why)) {
            notice.set(why, true);
            return;
        }
        const std::string name = tape.sd_name(socket);
        if (cart_was_in) {
            notice.set((socket == 0 ? "microSD: " : "SD: ") + name, false);
        } else {
            notice.show(false, "%s: %s; SD Dual in, powered on", socket == 0 ? "microSD" : "SD", name.c_str());
        }
    };
    // Snapshots (design/media.md "Snapshots"); the quick slot is one file in the user data folder
    const fs::path quick_path = user_dir / "quick.tvcs";
    std::error_code quick_ec;
    bool quick_exists = fs::exists(quick_path, quick_ec);
    const auto save_snapshot = [&](const std::string& path) {
        std::string why;
        if (tape.snapshot_save(path, why)) {
            quick_exists |= fs::path(path) == quick_path;
            notice.show(false, "snapshot saved: %s", fs::path(path).filename().string().c_str());
        } else {
            notice.set(why, true);
        }
    };
    const auto load_snapshot = [&](const std::string& path) {
        std::string why;
        if (!tape.snapshot_load(path, why)) {
            notice.set(why, true);
            return;
        }
        debugger->cancel_step();
        monitor->machine_changed();
        input.machine_replaced();
        sync_preset();
        notice.show(false, "snapshot loaded: %s", fs::path(path).filename().string().c_str());
    };
    const auto open_program = [&](const std::string& path, bool attach_only) {
        if (freetvc::HostTape::is_cart_file(path)) {
            insert_cart(path);
            return;
        }
        if (is_snapshot_file(path)) {
            load_snapshot(path);
            return;
        }
        // A raw image larger than any floppy is an SD card for the SD Dual
        std::error_code size_ec;
        if (freetvc::HostTape::is_disk_file(path) && fs::file_size(path, size_ec) > 720 * 1024 && !size_ec) {
            insert_sd(0, path);
            return;
        }
        if (freetvc::HostTape::is_disk_file(path) && attach_only) {
            insert_disk(0, path);
            return;
        }
        if (freetvc::HostTape::is_disk_file(path)) {
            std::string why;
            if (!tape.disk_autostart(path, why)) {
                notice.set(why, true);
                return;
            }
            debugger->cancel_step();
            monitor->machine_changed();
            input.machine_replaced();
            notice.show(false, "starting %s from A:", tape.media().program_name().c_str());
            return;
        }
        std::string why;
        const bool ok = attach_only ? tape.attach(path, why) : tape.autostart(path, why);
        if (ok && !attach_only) {
            notice.show(false, "starting %s", tape.media().cassette().attached_name().c_str());
        } else {
            notice.set(ok ? tape.describe() : why, !ok);
        }
        if (ok && !attach_only && debugger) {
            debugger->cancel_step();
        }
    };

    // The audio device's clock paces the machine; without a device, the wall clock does
    SDL_AudioStream* audio = nullptr;
    if (audio_wanted) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, static_cast<int>(machine.audio_rate())};
        audio = SDL_InitSubSystem(SDL_INIT_AUDIO)
                    ? SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr)
                    : nullptr;
        if (audio) {
            SDL_ResumeAudioStreamDevice(audio);
            SDL_SetAudioStreamGain(audio, static_cast<float>(volume) / 100.0f);
        } else {
            std::fprintf(stderr, "freetvc: no audio (%s); timing from the wall clock\n", SDL_GetError());
        }
    }
    const std::uint64_t queue_target = machine.audio_rate() * 60 / 1000;  // 60 ms of samples waiting for the device
    std::vector<float> samples;

    bool muted = false;
    bool rewinding = false;
    std::uint64_t rewind_next_ns = 0;
    tape.set_rewind(true);
    bool fast_tape = true;
    bool fast_disk = true;
    RomDownload rom_download;
    // A drive counts as active this long after its last step, byte or command (the light and fast disk)
    constexpr std::uint64_t kDiskActiveTicks = freetvc::AudioSynth::kTickRate / 4;
    bool menu_open = false;
    // The TVC256k++'s mouse: host motion gathered between frames, sent as one report per frame
    bool mouse_captured = false;
    float mouse_dx = 0.0f;
    float mouse_dy = 0.0f;
    unsigned mouse_buttons = 0;  // HID bits: b0 left, b1 right, b2 middle
    bool mouse_changed = false;
    const auto release_mouse = [&](const char* why) {
        SDL_SetWindowRelativeMouseMode(window, false);
        mouse_captured = false;
        mouse_dx = mouse_dy = 0.0f;
        mouse_buttons = 0;
        notice.show_text(false, why);
    };
    std::uint64_t wall_pending = 0;  // ticks owed to the wall clock (no audio device)
    std::uint64_t last_ns = SDL_GetTicksNS();
    std::uint64_t speed_since_ns = last_ns;
    std::uint64_t speed_ticks = 0;
    double speed_percent = -1.0;
    std::uint64_t skin_poll_ns = last_ns;
    fs::path pending_skin;
    long frames = 0;
    bool running = true;
    while (running) {
        // Fonts may only change between frames
        if (!pending_skin.empty()) {
            const std::string message = skins.load(pending_skin);
            if (message.empty()) {
                notice.show(false, "skin %s", skins.skin.name.c_str());
            } else {
                notice.set(message, true);
            }
            pending_skin.clear();
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // A key for the keyboard editor goes nowhere else
            if (keyboard_editor.waiting() && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && keyboard_editor.take_key(event.key.scancode)) {
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                open_program(event.drop.data, (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
            }
            // The TVC256k++'s mouse: a click on the picture captures the host mouse; Ctrl+Alt or leaving the window lets it go
            if (mouse_captured) {
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    release_mouse(N_("mouse released"));
                } else if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL) && (event.key.mod & SDL_KMOD_ALT)) {
                    release_mouse(N_("mouse released (Ctrl+Alt)"));
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    mouse_dx += event.motion.xrel;
                    mouse_dy += event.motion.yrel;
                    continue;
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    const unsigned bit = event.button.button == SDL_BUTTON_LEFT    ? 1u
                                         : event.button.button == SDL_BUTTON_RIGHT ? 2u
                                         : event.button.button == SDL_BUTTON_MIDDLE ? 4u
                                                                                    : 0u;
                    mouse_buttons = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? mouse_buttons | bit : mouse_buttons & ~bit;
                    mouse_changed = true;
                    continue;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && !bench && !menu_open &&
                       tape.card256_slot() >= 0 && tape.card256_host().usb() == freetvc::Card256Usb::Mouse) {
                int w = 0;
                int h = 0;
                SDL_GetWindowSize(window, &w, &h);
                if (event.button.y < static_cast<float>(h) - freetvc::sdl::strip_space(skins.skin, skins.scale)) {
                    SDL_SetWindowRelativeMouseMode(window, true);
                    mouse_captured = true;
                    mouse_changed = true;
                    notice.show_text(false, "mouse captured by the TVC256k++; Ctrl+Alt lets it go");
                    continue;
                }
            }
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                skins.scale = ui_scale(window);
                freetvc::sdl::apply_style(skins.skin, skins.scale, ImGui::GetStyle());
            }
            const bool key_event = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
            if (key_event) {
                // Reserved host keys never reach the TVC (design/ui.md "Play"); F9 is Bench's own there
                const SDL_Scancode sc = event.key.scancode;
                if (sc == SDL_SCANCODE_F10) {
                    if (!event.key.repeat) {
                        rewinding = event.type == SDL_EVENT_KEY_DOWN;
                        rewind_next_ns = 0;
                    }
                    continue;
                }
                if (!bench && sc == SDL_SCANCODE_F9) {
                    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                        if (event.key.mod & SDL_KMOD_SHIFT) {
                            if (quick_exists) {
                                load_snapshot(quick_path.string());
                            } else {
                                notice.show_text(true, "no quick snapshot yet (F9 saves one)");
                            }
                        } else {
                            save_snapshot(quick_path.string());
                        }
                    }
                    continue;
                }
                if (sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12 || sc == SDL_SCANCODE_PAUSE) {
                    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                        if (sc == SDL_SCANCODE_F11) {
                            SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
                        } else if (sc == SDL_SCANCODE_F12) {
                            set_bench(!bench);
                        } else {
                            toggle_hold();
                        }
                    }
                    continue;
                }
                // In Bench the TVC gets keys only while its Screen panel has focus; releases always pass
                if (event.type == SDL_EVENT_KEY_DOWN && (menu_open || io.WantTextInput || (bench && !keys_to_tvc))) {
                    continue;
                }
            }
            input.handle_event(event);
        }

        const std::uint64_t now_ns = SDL_GetTicksNS();
        if (now_ns - skin_poll_ns >= kSkinPollNs) {
            skin_poll_ns = now_ns;
            if (skins.changed_on_disk()) {
                pending_skin = skins.path;
            }
            if (pictures.changed_on_disk()) {
                use_picture(pictures.path);
            }
        }

        if (hub) {
            hub->service();
            if (mcp_host) {
                mcp_host->service();  // tool calls run here, on the GUI thread; a long run call holds the window
            }
            if (dzrp_host) {
                dzrp_host->service();
            }
            if (gdb_host) {
                gdb_host->service();
            }
            if (hub->quit_code()) {
                running = false;
            }
            if (held() != was_held) {
                was_held = held();
                if (was_held) {
                    notice.show(false, "monitor: stopped at %s", monitor->prompt().substr(4, 4).c_str());
                } else {
                    notice.show_text(false, "monitor: running");
                }
            }
        }

        {
            const std::lock_guard lock(file_choice.mutex);
            if (file_choice.path) {
                const std::string path = std::move(*file_choice.path);
                file_choice.path.reset();
                if (file_choice.purpose == FileChoice::Purpose::Symbols) {
                    run_command("ll \"" + path + "\"");
                } else if (file_choice.purpose == FileChoice::Purpose::Program) {
                    open_program(path, false);
                } else if (file_choice.purpose == FileChoice::Purpose::Cart) {
                    insert_cart(path);
                } else if (file_choice.purpose == FileChoice::Purpose::MultiCartFiles) {
                    // The programs are known; now where the image goes
                    multicart_files.assign(1, path);
                    multicart_files.insert(multicart_files.end(), file_choice.more.begin(), file_choice.more.end());
                    static const SDL_DialogFileFilter kImages[] = {{N_("MultiCart images (*.bin)"), "bin;BIN"}};
                    file_choice.showing = true;
                    file_choice.purpose = FileChoice::Purpose::MultiCartImage;
                    SDL_ShowSaveFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kImages, 1), 1, nullptr);
                } else if (file_choice.purpose == FileChoice::Purpose::MultiCartImage) {
                    const std::string out = fs::path(path).has_extension() ? path : path + ".bin";
                    if (tape.multicart_build(out, multicart_files, "en", error)) {
                        insert_cart(out);
                    } else {
                        notice.show(true, "MultiCart: %s", error.c_str());
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::SdImage) {
                    insert_sd(file_choice.drive, path);
                } else if (file_choice.purpose == FileChoice::Purpose::SdNewFiles) {
                    sd_new_files.assign(1, path);
                    sd_new_files.insert(sd_new_files.end(), file_choice.more.begin(), file_choice.more.end());
                    static const SDL_DialogFileFilter kCards[] = {{N_("SD card images (*.img)"), "img;IMG"}};
                    file_choice.showing = true;
                    file_choice.purpose = FileChoice::Purpose::SdNewImage;
                    SDL_ShowSaveFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kCards, 1), 1, nullptr);
                } else if (file_choice.purpose == FileChoice::Purpose::SdNewImage) {
                    const std::string out = fs::path(path).has_extension() ? path : path + ".img";
                    if (tape.sd_build(out, 64, sd_new_files, error)) {
                        insert_sd(0, out);
                    } else {
                        notice.show(true, "SD card image: %s", error.c_str());
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::SnapshotSave) {
                    save_snapshot(fs::path(path).has_extension() ? path : path + ".tvcs");
                } else if (file_choice.purpose == FileChoice::Purpose::SnapshotLoad) {
                    load_snapshot(path);
                } else if (file_choice.purpose == FileChoice::Purpose::Disk) {
                    insert_disk(file_choice.drive, path);
                } else if (file_choice.purpose == FileChoice::Purpose::RomDir) {
                    use_rom_dir(fs::absolute(path).lexically_normal().string());
                } else if (file_choice.purpose == FileChoice::Purpose::DiskRom) {
                    use_disk_rom(fs::absolute(path).lexically_normal().string());
                } else if (file_choice.purpose == FileChoice::Purpose::Pendrive) {
                    if (tape.card256_usb(path, error)) {
                        notice.show(false, "TVC256k++: pendrive %s", fs::path(path).filename().string().c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::PrinterFile) {
                    const bool text = fs::path(path).extension() == ".txt" || fs::path(path).extension() == ".TXT";
                    if (tape.printer_plug(text ? std::string() : path, text ? path : std::string(), error)) {
                        notice.show(false, text ? N_("printer to %s (text)") : N_("printer to %s (raw bytes)"), fs::path(path).filename().string().c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::SerialSend) {
                    if (tape.serial_queue_file(path, error)) {
                        notice.show(false, "serial: %s queued for the TVC", fs::path(path).filename().string().c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::SerialCapture) {
                    if (tape.serial_capture(path, error)) {
                        serial_capture_name = fs::path(path).filename().string();
                        if (tape.serial_peer_name() == "off") {
                            tape.serial_peer("on", error);
                        }
                        notice.show(false, "serial: capturing to %s", serial_capture_name.c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::VideoDir || file_choice.purpose == FileChoice::Purpose::ShotDir) {
                    const bool video = file_choice.purpose == FileChoice::Purpose::VideoDir;
                    const std::string dir = fs::absolute(path).lexically_normal().string();
                    (video ? video_dir_setting : screenshot_dir_setting) = dir;
                    save_settings({{video ? "video_dir" : "screenshot_dir", dir}});
                    notice.show(false, video ? N_("recordings go to %s") : N_("screenshots go to %s"), dir.c_str());
                } else if (file_choice.purpose == FileChoice::Purpose::SdCard) {
                    if (tape.nanosd_sd(path, error)) {
                        notice.show(false, "NanoSD: SD card %s", fs::path(path).filename().string().c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else if (file_choice.purpose == FileChoice::Purpose::Flash) {
                    if (tape.card256_flash(path, error)) {
                        card256_flash = fs::absolute(path).string();
                        notice.show(false, "TVC256k++: flash drive %s", fs::path(path).filename().string().c_str());
                    } else {
                        notice.set(error, true);
                    }
                } else {
                    const bool ok = file_choice.purpose == FileChoice::Purpose::Tape ? tape.tape_insert(path, error)
                                                                                       : tape.tape_record(path, error);
                    if (!ok) {
                        notice.set(error, true);
                    } else if (file_choice.purpose == FileChoice::Purpose::Tape && file_choice.play) {
                        tape.tape_play();
                    }
                }
            }
            if (!file_choice.error.empty()) {
                notice.show(true, "file dialog: %s", file_choice.error.c_str());
                file_choice.error.clear();
            }
        }

        if (mouse_captured && (tape.card256_slot() < 0 || tape.card256_host().usb() != freetvc::Card256Usb::Mouse)) {
            release_mouse(N_("mouse released: no mouse on a TVC256k++"));
        }
        if (mouse_captured) {
            const int dx = static_cast<int>(mouse_dx);
            const int dy = static_cast<int>(mouse_dy);
            mouse_dx -= static_cast<float>(dx);
            mouse_dy -= static_cast<float>(dy);
            if (dx != 0 || dy != 0 || mouse_changed) {
                tape.card256_mouse(dx, dy, mouse_buttons, error);
                mouse_changed = false;
            }
        }

        std::uint64_t ran = 0;
        // Rewind: while F10 is held the machine stands still and steps back one kept second every 200 ms
        if (rewinding && now_ns >= rewind_next_ns) {
            rewind_next_ns = now_ns + 200'000'000;
            std::string why;
            if (tape.rewind(1, why)) {
                debugger->cancel_step();
                input.machine_replaced();
                notice.set(tape.rewind_describe(), false);
            } else {
                notice.set(why, true);
            }
        }
        const bool stopped = held() || rewinding;
        if (tape.media().busy() && !warping) {
            warping = true;
            input.machine_replaced();
            upload_frame(*display, texture, blank);
        }
        if (!warping) {
            input.update(machine);
            tape.media().service(machine);
        }
        if (warping) {
            // Unpaced until the program starts; the slices end at frame starts (Media::run_slice). Not counted as speed.
            const std::uint64_t until_ns = now_ns + 12'000'000;
            while (tape.media().busy() && !stopped && !held() && SDL_GetTicksNS() < until_ns) {
                hub->run(kMaxCatchUp);
            }
            samples.resize(machine.audio_available());
            machine.read_audio(samples);
            if (!tape.media().busy()) {
                warping = false;
                wall_pending = 0;
                speed_since_ns = SDL_GetTicksNS();
                speed_ticks = 0;
                speed_percent = -1.0;
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), false);
                }
                const bool ok = tape.media().state() == freetvc::AutostartState::Done;
                notice.set(tape.media().autostart_result(), !ok);
            }
        } else if (fast_tape && tape.media().deck().moving() && !stopped) {
            // Fast tape: unpaced while the tape moves, the screen still shown; the sound is dropped
            const std::uint64_t until_ns = now_ns + 12'000'000;
            while (tape.media().deck().moving() && !held() && SDL_GetTicksNS() < until_ns) {
                ran += hub->run(kMaxCatchUp);
            }
            samples.resize(machine.audio_available());
            machine.read_audio(samples);
            wall_pending = 0;
        } else if (fast_disk && tape.disk_card_on() && tape.drives().any_active(machine.ticks(), kDiskActiveTicks) && !stopped) {
            // Fast disk (cards.md): the same, while a drive was active in the last 0.25 s of machine time
            const std::uint64_t until_ns = now_ns + 12'000'000;
            while (tape.drives().any_active(machine.ticks(), kDiskActiveTicks) && !held() && SDL_GetTicksNS() < until_ns) {
                ran += hub->run(kMaxCatchUp);
            }
            samples.resize(machine.audio_available());
            machine.read_audio(samples);
            wall_pending = 0;
        } else if (audio) {
            const auto queued = static_cast<std::uint64_t>(std::max(SDL_GetAudioStreamQueued(audio), 0)) / sizeof(float);
            if (!stopped && queued < queue_target) {
                const std::uint64_t ticks = (queue_target - queued) * kTicksPerSecond / machine.audio_rate() + 1;
                ran = hub->run(std::min(ticks, kMaxCatchUp));
            }
            samples.resize(machine.audio_available());
            machine.read_audio(samples);
            if (!samples.empty()) {
                SDL_PutAudioStreamData(audio, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
            }
        } else if (!stopped) {
            wall_pending = std::min(wall_pending + (now_ns - last_ns) * kTicksPerSecond / 1'000'000'000, kMaxCatchUp);
            if (wall_pending > 0) {
                ran = hub->run(wall_pending);
                wall_pending -= std::min(ran, wall_pending);
            }
            samples.resize(machine.audio_available());
            machine.read_audio(samples);  // discarded: keeps the buffer from filling
        }
        if (recorder.recording()) {
            recorder.audio(samples);
        }
        last_ns = now_ns;
        speed_ticks += ran;
        if (stopped) {
            speed_since_ns = now_ns;
            speed_ticks = 0;
            speed_percent = -1.0;
        } else if (now_ns - speed_since_ns >= 1'000'000'000) {
            speed_percent = 100.0 * static_cast<double>(speed_ticks) / static_cast<double>(kTicksPerSecond) /
                            (static_cast<double>(now_ns - speed_since_ns) / 1e9);
            speed_since_ns = now_ns;
            speed_ticks = 0;
        }
        if (!warping) {
            upload_frame(*display, texture, machine.framebuffer());
        }
        for (std::string& line : tape.take_messages()) {
            notice.set(std::move(line), false);
        }

        display->new_frame();
        ImGui::NewFrame();
        freetvc::sdl::PlayStatus status;
        status.screen = texture;
        if (splash_on && !bench) {
            const std::uint64_t t = SDL_GetTicksNS();
            if (splash_from_ns == 0) {
                splash_from_ns = t;
            }
            if (!splash_drawn && !warping) {
                const auto& px = machine.framebuffer().pixels;
                splash_drawn = std::any_of(px.begin(), px.end(), [](std::uint8_t c) { return c != 0 && c != freetvc::Framebuffer::kBlank; });
            }
            if (splash_fade_ns == 0 && !warping && t - splash_from_ns >= kSplashNs && (splash_drawn || t - splash_from_ns >= kSplashMaxNs)) {
                splash_fade_ns = t;
            }
            const float alpha = splash_fade_ns == 0 ? 1.0f : 1.0f - static_cast<float>(t - splash_fade_ns) / static_cast<float>(kSplashFadeNs);
            if (alpha <= 0.0f) {
                splash_on = false;
            } else {
                status.splash = splash;
                status.splash_size = splash_size;
                status.splash_alpha = alpha;
            }
        } else if (bench) {
            splash_on = false;
        }
        status.presets = freetvc::kPresets;
        status.preset = preset_index;
        status.paused = stopped;
        status.speed_percent = speed_percent;
        status.mapping = input.mapping();
        status.layouts = keymaps;
        status.layout = input.keymap().name;
        status.audio = audio != nullptr;
        status.muted = muted;
        status.fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        status.full_raster = full_raster;
        status.skins = skins.list;
        status.languages = languages;
        status.language = freetvc::sdl::current_language();
        status.skin_name = skins.path.empty() ? std::string_view{} : std::string_view(skins.skin.name);
        status.notice = notice.text;
        status.notice_is_error = notice.error;
        status.notice_age = static_cast<double>(now_ns - notice.since_ns) / 1e9;
        status.scale = skins.scale;
        status.cassette = tape.media().cassette().attached_name();
        status.recording = recorder.recording();
        status.record_seconds = recorder.seconds();
        {
            const std::lock_guard lock(file_choice.mutex);
            status.can_browse = !file_choice.showing;
        }
        status.bench = bench;
        const freetvc::TapeDeck& deck = tape.media().deck();
        const freetvc::TapeState tape_state = deck.state();
        const std::uint64_t tape_seconds = deck.position() / freetvc::kTapeTicksPerSecond;
        char tape_counter[16];
        std::snprintf(tape_counter, sizeof tape_counter, "%llu:%02llu", static_cast<unsigned long long>(tape_seconds / 60),
                      static_cast<unsigned long long>(tape_seconds % 60));
        status.tape_loaded = deck.loaded();
        status.tape_state = tape_state == freetvc::TapeState::Playing ? "play" : tape_state == freetvc::TapeState::Recording ? "rec" : "stop";
        status.tape_counter = tape_counter;
        status.tape_moving = deck.moving();
        status.tape_recording = tape_state == freetvc::TapeState::Recording;
        status.fast_tape = fast_tape;
        status.fast_disk = fast_disk;
        status.gamecard_slot = tape.gamecard_slot();
        status.multichip_slot = tape.multichip_slot();
        status.twodfx_slot = tape.twodfx_slot();
        const std::string sid_model = tape.sid_card_model();
        status.sid_card = sid_model == "6581" ? 1 : sid_model == "8580" ? 2 : 0;
        status.card256_slot = tape.card256_slot();
        const freetvc::Card256Host& card256_host = tape.card256_host();
        status.card256_usb = static_cast<int>(card256_host.usb());
        const std::string pendrive_name = card256_host.pendrive_dir().filename().string();
        status.card256_pendrive = pendrive_name;
        status.card256_flash = card256_host.has_flash() ? std::string_view(card256_host.flash_name()) : std::string_view();
        status.nanosd_slot = tape.nanosd_slot();
        status.printer = tape.printer_plugged();
        printer_name = tape.printer_setting();
        status.printer_file = printer_name == "on" || printer_name == "off" ? std::string_view() : std::string_view(printer_name);
        if (const auto slash = status.printer_file.find_last_of("/\\"); slash != std::string_view::npos) {
            status.printer_file.remove_prefix(slash + 1);
        }
        status.serial_slot = tape.serial_slot();
        const std::string serial_peer = tape.serial_peer_name();
        status.serial_line = serial_peer;
        status.serial_connected = machine.serial_line() && machine.serial_line()->connected();
        status.serial_capture = serial_capture_name;
        const std::string sd_name = tape.nanosd_sd_dir().filename().string();
        status.nanosd_sd = sd_name;
        status.mouse_captured = mouse_captured;
        status.sddual = tape.sddual_on();
        const std::array<std::string, 2> sd_names{tape.sd_name(0), tape.sd_name(1)};
        status.sd_cards = {sd_names[0], sd_names[1]};
        std::array<std::string, freetvc::kCardSlots> slot_cards;
        for (unsigned s = 0; s < freetvc::kCardSlots; ++s) {
            switch (machine.config().cards[s].kind) {
            case freetvc::CardKind::Hbf:
                slot_cards[s] = freetvc::sdl::tr("HBF disk card");
                break;
            case freetvc::CardKind::GameCard:
                slot_cards[s] = freetvc::sdl::tr("GameCard");
                break;
            case freetvc::CardKind::Card256:
                slot_cards[s] = freetvc::sdl::tr("TVC256k++");
                break;
            case freetvc::CardKind::Multichip:
                slot_cards[s] = freetvc::sdl::tr("Multichip sound card");
                break;
            case freetvc::CardKind::NanoSd:
                slot_cards[s] = freetvc::sdl::tr("NanoSD");
                break;
            case freetvc::CardKind::Serial:
                slot_cards[s] = freetvc::sdl::tr("Serial card");
                break;
            case freetvc::CardKind::TwoDfx:
                slot_cards[s] = freetvc::sdl::tr("2dfx graphics card");
                break;
            case freetvc::CardKind::None:
                break;
            }
            status.slot_cards[s] = slot_cards[s];
            status.slot_kinds[s] = machine.config().cards[s].kind;
            status.disk_rom = disk_rom_path;
        }
        const std::string cart_name = tape.cart_name();
        status.cartridge = cart_name;
        status.quick_snapshot = quick_exists;
        std::array<std::string, freetvc::FloppyDrives::kDrives> disk_names;
        status.disk_card = tape.disk_card_on();
        status.disk_busy = -1;
        for (unsigned d = 0; d < freetvc::FloppyDrives::kDrives; ++d) {
            disk_names[d] = tape.disk_name(d);
            status.disks[d] = disk_names[d];
            if (status.disk_card && tape.drives().disk(d) && tape.drives().active(d, machine.ticks(), kDiskActiveTicks)) {
                status.disk_busy = static_cast<int>(d);
            }
        }
        bench_view.console_output(hub->take_local_output());
        status.side_panel = settings_view.width(skins.skin, skins.fonts, skins.scale, ImGui::GetMainViewport()->Size.x);
        status.bottom_panel = keyboard_editor.height(skins.skin, skins.scale);
        status.picture = [&](ImDrawList* draw, ImVec2 min, ImVec2 max, freetvc::ScreenArea area) {
            return display->draw_picture(draw, min, max, area);
        };
        status.settings = settings_view.is_open();
        freetvc::sdl::PlayActions actions;
        freetvc::sdl::BenchActions bench_actions;
        if (bench) {
            bench_actions = bench_view.draw(status, *monitor, skins.skin, skins.fonts, actions);
        } else {
            actions = freetvc::sdl::draw_play(status, skins.skin, skins.fonts);
        }
        // The settings sheet: Bench keeps its run bar above it
        freetvc::sdl::SettingsStatus sheet;
        sheet.file = settings_file;
        sheet.portable = settings.portable;
        sheet.rom_source = rom_source;
        sheet.rom_dir = rom_dir_setting;
        sheet.rom_dir_given = rom_dir_given;
        sheet.downloading = rom_download.running();
        sheet.disk_rom = disk_rom_path;
        sheet.disk_rom_given = disk_rom_given;
        sheet.preset_given = preset_given;
        sheet.skin_given = skin_given;
        sheet.keymap_given = keymap_given;
        sheet.language_given = language_given;
        sheet.audio_at_start = audio_at_start;
        sheet.audio_given = audio_given;
        sheet.volume = volume;
        sheet.fullscreen_at_start = fullscreen_at_start;
        sheet.display = display->name();
        sheet.effects = display->effects();
        sheet.renderer_sdl = renderer_option == "sdl";
        sheet.pictures = pictures.list;
        sheet.picture = &pictures.picture;
        sheet.picture_given = picture_given;
        sheet.picture_edited = !pictures.path.empty() && pictures.path.parent_path() == user_dir / "pictures";
        sheet.picture_error = picture_error;
        sheet.colour_switch = machine.config().colour;
        for (std::size_t i = 0; i < servers.size(); ++i) {
            sheet.servers[i] = {servers[i].on, static_cast<int>(servers[i].port), server_port(i), servers[i].given, servers[i].error};
        }
        sheet.mcp_token = mcp_token_now;
        sheet.mcp_allow_monitor = mcp_allow_monitor;
        sheet.dzrp_banks = dzrp_memory == freetvc::remote::DzrpMemory::TvcBanks;
        const float sheet_top =
            ImGui::GetMainViewport()->Pos.y + (bench ? (skins.skin.strip_height + skins.skin.groove_width) * skins.scale : 0.0f);
        const freetvc::sdl::KeyboardEditorActions editor_actions =
            keyboard_editor.draw(input.keymap(), input.mapping() == freetvc::sdl::KeyMapping::Positional,
                                 keymap_path.parent_path() == user_dir / "keymaps", skins.skin, skins.fonts, skins.scale, status.side_panel);
        actions.menu_open |= editor_actions.keys_held;
        sheet.layouts = keymaps;
        sheet.layout = input.keymap().name;
        sheet.layout_given = keys_given;
        const freetvc::sdl::SettingsActions sheet_actions = settings_view.draw(status, sheet, skins.skin, skins.fonts, sheet_top, actions);
        actions.menu_open |= about_view.draw(*display, base_dir / "about" / "lamerz-logo.png", display->name(), skins.skin, skins.fonts, skins.scale);
        ImGui::Render();

        display->prepare_picture(texture, machine.config().colour, machine.frame_count());
        display->present(skins.skin.frame);

        if (actions.menu_open && !menu_open) {
            input.release_all();
        }
        menu_open = actions.menu_open;
        if (actions.toggle_settings) {
            settings_view.toggle();
        }
        if (actions.open_about) {
            about_view.open();
        }
        if (sheet_actions.choose_rom_dir || sheet_actions.choose_disk_rom) {
            static const SDL_DialogFileFilter kRoms[] = {{N_("ROM images (*.rom, *.bin)"), "rom;bin;ROM;BIN"}, {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = sheet_actions.choose_rom_dir ? FileChoice::Purpose::RomDir : FileChoice::Purpose::DiskRom;
            }
            if (sheet_actions.choose_rom_dir) {
                SDL_ShowOpenFolderDialog(&FileChoice::done, &file_choice, window, nullptr, false);
            } else {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kRoms, 2), 2, nullptr, false);
            }
        }
        if (sheet_actions.default_rom_dir) {
            use_rom_dir("");
        }
        if (!sheet_actions.disk_rom.empty()) {
            use_disk_rom(sheet_actions.disk_rom);
        }
        if (sheet_actions.volume >= 0) {
            volume = sheet_actions.volume;
            if (audio && !muted) {
                SDL_SetAudioStreamGain(audio, static_cast<float>(volume) / 100.0f);
            }
            save_settings({{"volume", std::to_string(volume)}});
        }
        if (sheet_actions.audio_at_start >= 0) {
            audio_at_start = sheet_actions.audio_at_start == 1;
            save_settings({{"audio", audio_at_start ? "yes" : "no"}});
        }
        if (sheet_actions.fullscreen_at_start >= 0) {
            fullscreen_at_start = sheet_actions.fullscreen_at_start == 1;
            save_settings({{"fullscreen", fullscreen_at_start ? "yes" : "no"}});
        }
        if (sheet_actions.mcp_allow_monitor >= 0) {
            mcp_allow_monitor = sheet_actions.mcp_allow_monitor == 1;
            if (mcp) {
                mcp->set_allow_monitor_command(mcp_allow_monitor);
            }
            save_settings({{"mcp_allow_monitor", mcp_allow_monitor ? "yes" : "no"}});
        }
        if (sheet_actions.dzrp_banks >= 0) {
            dzrp_memory = sheet_actions.dzrp_banks == 1 ? freetvc::remote::DzrpMemory::TvcBanks : freetvc::remote::DzrpMemory::Zx48k;
            if (servers[freetvc::sdl::kDzrpServer].on) {
                set_server(freetvc::sdl::kDzrpServer, true);  // DeZog reconnects to the new memory model
            }
            save_settings({{"dzrp_banks", sheet_actions.dzrp_banks == 1 ? "yes" : "no"}});
        }
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (sheet_actions.server_on[i] < 0 && sheet_actions.server_port[i] < 0) {
                continue;
            }
            if (sheet_actions.server_port[i] >= 0) {
                servers[i].port = sheet_actions.server_port[i];
            }
            const bool on = sheet_actions.server_on[i] >= 0 ? sheet_actions.server_on[i] == 1 : servers[i].on;
            if (on || servers[i].on) {
                set_server(i, on);
            }
            if (i == freetvc::sdl::kMcpServer) {
                bench_view.set_mcp(mcp.get(), mcp_host ? "http://127.0.0.1:" + std::to_string(mcp_host->http_port()) + "/mcp" : std::string());
            }
            constexpr const char* kKeys[] = {"monitor", "mcp", "dzrp", "gdb"};
            save_settings({{kKeys[i], !on                                ? std::string("off")
                                      : servers[i].port == default_ports[i] ? std::string("on")
                                                                            : std::to_string(servers[i].port)}});
        }
        if (editor_actions.bind || editor_actions.clear) {
            freetvc::sdl::Keymap keymap = input.keymap();
            if (editor_actions.bind) {
                keymap.bind(editor_actions.bind->scancode, editor_actions.bind->key, editor_actions.bind->shift);
            } else {
                keymap.clear(*editor_actions.clear);
            }
            input.set_keymap(std::move(keymap));
            save_keymap();
        }
        if (editor_actions.reset) {
            std::error_code ec;
            const std::string name = input.keymap().name;
            fs::remove(keymap_path, ec);
            if (const auto shipped = find_keymap(name)) {
                use_keymap(*shipped);
            }
        }
        if (actions.edit_layout) {
            keyboard_editor.open();
            settings_view.close();
        }
        if (!actions.layout.empty()) {
            use_keymap(actions.layout);
            save_settings({{"keys", input.keymap().name}});
        }
        if (sheet_actions.edit_layout) {
            keyboard_editor.open();
            settings_view.close();  // the editor wants the window's width
        }
        if (!sheet_actions.layout.empty()) {
            use_keymap(sheet_actions.layout);
            save_settings({{"keys", input.keymap().name}});
        }
        if (!sheet_actions.picture.empty()) {
            use_picture(sheet_actions.picture);
            save_settings({{"picture", pictures.picture.name}});
        }
        if (sheet_actions.picture_edit) {
            pictures.picture = *sheet_actions.picture_edit;
            display->set_picture(pictures.picture, picture_error);
            if (sheet_actions.picture_edit_done) {
                // The values go into the user data folder's copy, which then shadows the shipped file
                const fs::path copy = user_dir / "pictures" / (pictures.picture.name + ".picture");
                std::error_code ec;
                fs::create_directories(copy.parent_path(), ec);
                std::ofstream out(copy, std::ios::binary | std::ios::trunc);
                out << "; " << pictures.picture.name << " with the values set in freeTVC's settings window\n"
                    << freetvc::sdl::picture_text(pictures.picture);
                out.close();
                if (out) {
                    use_picture(copy);
                } else {
                    notice.show(true, "cannot write '%s'", copy.string().c_str());
                }
            }
        }
        if (sheet_actions.picture_reset) {
            std::error_code ec;
            const std::string name = pictures.picture.name;
            fs::remove(pictures.path, ec);
            if (const auto shipped = pictures.find(name)) {
                use_picture(*shipped);
            }
        }
        if (sheet_actions.colour_switch >= 0) {
            machine.set_colour_switch(sheet_actions.colour_switch == 1);
            save_settings({{"colour_switch", sheet_actions.colour_switch == 1 ? "colour" : "bw"}});
        }
        if (sheet_actions.renderer_sdl >= 0) {
            renderer_option = sheet_actions.renderer_sdl == 1 ? "sdl" : "gl";
            save_settings({{"renderer", renderer_option}});
        }
        if (sheet_actions.copy_mcp_token) {
            SDL_SetClipboardText(mcp_token_now.c_str());
            notice.show_text(false, N_("MCP token copied"));
        }
        if (rom_reload >= 0) {
            actions.preset = rom_reload;  // the ROMs from the new folder: the same preset, powered on
            rom_reload = -1;
        }
        if (actions.toggle_pause) {
            toggle_hold();
        }
        if (actions.toggle_bench || bench_actions.play) {
            set_bench(!bench);
        }
        const bool tvc_keys = !bench || bench_actions.keys_to_tvc;
        if (keys_to_tvc && !tvc_keys) {
            input.release_all();
        }
        keys_to_tvc = tvc_keys;

        // Bench commands go through the monitor (design/ui.md "Bench": control)
        if (bench_actions.pause && !held()) {
            bench_view.console_output(monitor->enter("pause"));
        }
        if (held()) {
            const char* command = bench_actions.run ? "x" : bench_actions.step ? "z" : bench_actions.over ? "n" : bench_actions.out ? "ret" : nullptr;
            if (command) {
                run_command(command);
            } else if (bench_actions.run_to) {
                char line[16];
                std::snprintf(line, sizeof line, "until %04X", static_cast<unsigned>(*bench_actions.run_to));
                run_command(line);
            }
        }
        if (bench_actions.toggle_checkpoint) {
            toggle_checkpoint(*debugger, *bench_actions.toggle_checkpoint);
        }
        if (held() && (bench_actions.line_step || bench_actions.line_next)) {
            run_command(bench_actions.line_step ? "lstep" : "lnext");
        }
        if (bench_actions.load_symbols) {
            static const SDL_DialogFileFilter kSymbols[] = {
                {N_("Debug and label files (*.sld, *.lst, *.sym, *.noi, *.cdb, *.map, *.txt)"), "sld;lst;sym;noi;cdb;map;txt;SLD;LST;SYM;NOI;CDB;MAP;TXT"},
                {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = FileChoice::Purpose::Symbols;
            }
            SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kSymbols, 2), 2, nullptr, false);
        }
        for (const std::string& line : bench_actions.commands) {
            run_command(line);
        }
        for (std::string& line : bench_actions.console_lines) {
            bench_view.console_command(held() ? monitor->prompt() : freetvc::sdl::tr("(running) "), line);
            hub->submit_local(console_client, std::move(line));
        }
        debugger->set_heat(bench && bench_actions.heat);
        debugger->set_events(bench && bench_actions.events);
        if (actions.preset >= 0) {
            const freetvc::Preset& wanted = freetvc::kPresets[static_cast<std::size_t>(actions.preset)];
            const freetvc::RomPlace* place = nullptr;
            if (auto new_config = freetvc::find_preset_roms(wanted, rom_search, error, &place)) {
                rom_source = place->dir.string() + " (" + place->why + ")";
                // The cartridge and the cards stay in across a change of board and ROM set
                new_config->roms.cart = machine.config().roms.cart;
                new_config->roms.cart_type = machine.config().roms.cart_type;
                new_config->roms.cart_bank = machine.config().roms.cart_bank;
                new_config->cards = machine.config().cards;
                new_config->colour = machine.config().colour;
                bool card256_dropped = false;
                if (new_config->model == freetvc::Model::Tvc32kHbm) {
                    for (freetvc::CardConfig& c : new_config->cards) {
                        if (c.kind == freetvc::CardKind::Card256) {
                            c = {};
                            card256_dropped = true;
                        }
                    }
                }
                machine.configure(*new_config);
                if (monitor) {
                    monitor->machine_changed();
                    show_machine();
                }
                if (mcp) {
                    mcp->set_machine_name(std::string(wanted.name));
                }
                preset_index = static_cast<std::size_t>(actions.preset);
                set_title();
                const std::string name(wanted.name);
                if (card256_dropped) {
                    notice.show(false, "%s powered on, without the TVC256k++ (no other RAM card beside it)", name.c_str());
                } else {
                    notice.show(false, "%s powered on", name.c_str());
                }
            } else {
                std::fprintf(stderr, "freetvc: %s\n", error.c_str());
                notice.show(true, "%s (the places searched are on stderr)", error.substr(0, error.find("; searched")).c_str());  // the strip has one line
            }
            input.release_all();
        }
        if (actions.reset) {
            tape.media().cancel();
            debugger->cancel_step();
            machine.reset();
            notice.show_text(false, "reset");
        }
        if (actions.fetch_roms && !rom_download.running()) {
            notice.show_text(false, "downloading the ROMs from tvc.homeserver.hu...");
            rom_download.start(rom_dir_setting.empty() ? user_dir / "roms" : fs::path(rom_dir_setting));
        }
        if (rom_download.finished()) {
            if (rom_download.ok) {
                notice.show_text(false, rom_download.dir == user_dir / "roms" ? N_("ROMs downloaded into the user data folder")
                                                                              : N_("ROMs downloaded into the ROM folder (rom_dir)"));
            } else {
                notice.set(rom_download.result, true);
            }
            // Cards that went in without their files take them now; a changed card powers the machine on
            if (rom_download.ok && tape.refresh_card_roms()) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                notice.show_text(false, "ROMs downloaded; the cards took theirs and the machine powered on");
            }
            for (std::string& line : tape.take_messages()) {
                notice.set(std::move(line), true);
            }
        }
        if (actions.power_cycle) {
            tape.media().cancel();
            machine.power_on();
            show_machine();
            input.machine_replaced();
            notice.show_text(false, "power cycled");
        }
        if (actions.load_program || actions.tape_insert || actions.tape_insert_play || actions.tape_record || actions.cart_insert) {
            static const SDL_DialogFileFilter kPrograms[] = {{N_("TVC programs (*.cas, *.wav, *.ttp, *.crt, *.rom, *.dsk, *.img)"), "cas;CAS;wav;WAV;ttp;TTP;crt;CRT;rom;ROM;dsk;DSK;img;IMG"}, {N_("All files"), "*"}};
            static const SDL_DialogFileFilter kTapes[] = {{N_("Tapes (*.wav, *.ttp, *.cas)"), "wav;WAV;ttp;TTP;cas;CAS"}, {N_("All files"), "*"}};
            static const SDL_DialogFileFilter kCarts[] = {{N_("Cartridges (*.crt, *.rom, *.bin)"), "crt;CRT;rom;ROM;bin;BIN"}, {N_("All files"), "*"}};
            static const SDL_DialogFileFilter kWav[] = {{N_("WAV files (*.wav)"), "wav;WAV"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.play = actions.tape_insert_play;
                file_choice.purpose = actions.load_program                                ? FileChoice::Purpose::Program
                                      : actions.tape_insert || actions.tape_insert_play ? FileChoice::Purpose::Tape
                                      : actions.cart_insert                             ? FileChoice::Purpose::Cart
                                                                                        : FileChoice::Purpose::Record;
            }
            if (actions.tape_record) {
                SDL_ShowSaveFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kWav, 1), 1, nullptr);
            } else {
                const SDL_DialogFileFilter* filters = actions.load_program ? kPrograms : actions.cart_insert ? kCarts : kTapes;
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(filters, 2), 2, nullptr, false);
            }
        }
        if (actions.snapshot_save || actions.snapshot_load) {
            static const SDL_DialogFileFilter kSnapshots[] = {{N_("Snapshots (*.tvcs, WinTVC *.tvs)"), "tvcs;TVCS;tvs;TVS"}, {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = actions.snapshot_save ? FileChoice::Purpose::SnapshotSave : FileChoice::Purpose::SnapshotLoad;
            }
            if (actions.snapshot_save) {
                SDL_ShowSaveFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kSnapshots, 1), 1, nullptr);
            } else {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kSnapshots, 2), 2, nullptr, false);
            }
        }
        if (actions.quick_save) {
            save_snapshot(quick_path.string());
        }
        if (actions.quick_load) {
            load_snapshot(quick_path.string());
        }
        if (actions.toggle_card256) {
            const bool on = tape.card256_slot() < 0;
            if (tape.card256(on, freetvc::kCardSlots, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "TVC256k++ in slot %d (EXT %d); powered on", tape.card256_slot(), tape.card256_slot());
                } else {
                    notice.show_text(false, "TVC256k++ out; powered on");
                }
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), true);
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.card256_mouse || actions.card256_unplug) {
            tape.card256_usb(actions.card256_mouse ? "mouse" : "none", error);
            notice.show_text(false, actions.card256_mouse ? N_("TVC256k++: mouse on the USB port; click the screen to capture it")
                                                          : N_("TVC256k++: USB port empty"));
        }
        if (actions.card256_pendrive || actions.card256_flash) {
            static const SDL_DialogFileFilter kFlash[] = {{N_("littlefs images (*.bin)"), "bin;BIN"}, {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = actions.card256_pendrive ? FileChoice::Purpose::Pendrive : FileChoice::Purpose::Flash;
            }
            if (actions.card256_pendrive) {
                SDL_ShowOpenFolderDialog(&FileChoice::done, &file_choice, window, nullptr, false);
            } else {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kFlash, 2), 2, nullptr, false);
            }
        }
        // The card's own slot, else the first free one
        const auto free_slot = [&](unsigned preferred) {
            if (machine.config().cards[preferred].kind == freetvc::CardKind::None) {
                return preferred;
            }
            for (unsigned s = 0; s < freetvc::kCardSlots; ++s) {
                if (machine.config().cards[s].kind == freetvc::CardKind::None) {
                    return s;
                }
            }
            return preferred;
        };
        if (actions.toggle_gamecard) {
            const bool on = tape.gamecard_slot() < 0;
            const unsigned slot = on ? free_slot(freetvc::kGameCardSlot) : freetvc::kGameCardSlot;
            if (tape.gamecard(on, slot, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "GameCard in slot %d; powered on", static_cast<int>(slot));
                } else {
                    notice.show_text(false, "GameCard out; powered on");
                }
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), true);
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.toggle_nanosd) {
            const bool on = tape.nanosd_slot() < 0;
            if (tape.nanosd(on, freetvc::kNanoSdSlot, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "NanoSD in slot %d; powered on", static_cast<int>(freetvc::kNanoSdSlot));
                } else {
                    notice.show_text(false, "NanoSD out; powered on");
                }
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), true);
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.toggle_sddual) {
            const bool on = !tape.sddual_on();
            if (tape.sddual(on, false, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                notice.show_text(false, on ? N_("SD Dual in; powered on (EXT 2: VT-DOS CLI, EXT 3: file manager)") : N_("SD Dual out; powered on"));
            } else {
                notice.show(true, "SD Dual: %s", error.c_str());
            }
        }
        if (actions.sd_eject >= 0) {
            tape.sd_eject(static_cast<unsigned>(actions.sd_eject));
            notice.show_text(false, actions.sd_eject == 0 ? N_("microSD card out") : N_("SD card out"));
        }
        if (actions.sd_insert >= 0 || actions.sd_new) {
            static const SDL_DialogFileFilter kCards[] = {{N_("SD card images (*.img)"), "img;IMG"}, {N_("All files"), "*"}};
            static const SDL_DialogFileFilter kAny[] = {{N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.drive = actions.sd_insert >= 0 ? static_cast<unsigned>(actions.sd_insert) : 0;
                file_choice.purpose = actions.sd_new ? FileChoice::Purpose::SdNewFiles : FileChoice::Purpose::SdImage;
            }
            if (actions.sd_new) {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kAny, 1), 1, nullptr, true);
            } else {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kCards, 2), 2, nullptr, false);
            }
        }
        if (actions.printer_file || actions.serial_send_file || actions.serial_capture) {
            static const SDL_DialogFileFilter kPrinter[] = {{N_("Printer output (*.prn raw, *.txt text)"), "prn;PRN;txt;TXT"}, {N_("All files"), "*"}};
            static const SDL_DialogFileFilter kAny[] = {{N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = actions.printer_file       ? FileChoice::Purpose::PrinterFile
                                      : actions.serial_send_file ? FileChoice::Purpose::SerialSend
                                                                 : FileChoice::Purpose::SerialCapture;
            }
            if (actions.serial_send_file) {
                SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kAny, 1), 1, nullptr, false);
            } else {
                const int count = actions.printer_file ? 2 : 1;
                SDL_ShowSaveFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(actions.printer_file ? kPrinter : kAny, count),
                                       count, nullptr);
            }
        }
        if (actions.printer_unplug) {
            tape.printer_unplug();
            notice.show_text(false, "no printer: an LPRINT waits after its first character until STOP");
        }
        if (actions.serial_capture_stop) {
            tape.serial_capture("", error);
            serial_capture_name.clear();
            notice.show_text(false, "serial: capture stopped");
        }
        if (actions.serial_line >= 0) {
            if (tape.serial_peer(actions.serial_line == 0 ? "off" : "listen", error)) {
                if (actions.serial_line == 0) {
                    notice.show_text(false, "serial cable unplugged");
                } else {
                    notice.show(false, "serial cable to TCP 127.0.0.1:%s", tape.serial_peer_name().substr(7).c_str());
                }
            } else {
                notice.show(true, "serial: %s", error.c_str());
            }
        }
        if (actions.toggle_serial) {
            const bool on = tape.serial_slot() < 0;
            const unsigned slot = on ? free_slot(freetvc::kSerialSlot) : freetvc::kSerialSlot;
            if (tape.serial_card(on, slot, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "serial card in slot %d; powered on", static_cast<int>(slot));
                } else {
                    notice.show_text(false, "serial card out; powered on");
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.nanosd_eject) {
            tape.nanosd_sd("none", error);
            notice.show_text(false, "NanoSD: SD card out");
        }
        if (actions.multicart_new) {
            static const SDL_DialogFileFilter kParts[] = {{N_("Programs and carts (*.cas, *.crt, *.rom, *.bin)"), "cas;CAS;crt;CRT;rom;ROM;bin;BIN"},
                                                          {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = FileChoice::Purpose::MultiCartFiles;
            }
            SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kParts, 2), 2, nullptr, true);
        }
        if (actions.nanosd_sd) {
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = FileChoice::Purpose::SdCard;
            }
            SDL_ShowOpenFolderDialog(&FileChoice::done, &file_choice, window, nullptr, false);
        }
        if (actions.sid_card >= 0) {
            const char* const model = actions.sid_card == 1 ? "6581" : actions.sid_card == 2 ? "8580" : "off";
            if (tape.sid_card(model, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (actions.sid_card == 0) {
                    notice.show_text(false, "SID card out; powered on");
                } else {
                    notice.show(false, "SID card (MOS %s) at ports E0h-FFh; powered on", model);
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.toggle_twodfx) {
            const bool on = tape.twodfx_slot() < 0;
            if (tape.twodfx(on, freetvc::kCardSlots, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "2dfx graphics card in slot %d; powered on", tape.twodfx_slot());
                } else {
                    notice.show_text(false, "2dfx graphics card out; powered on");
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.toggle_multichip) {
            const bool on = tape.multichip_slot() < 0;
            if (tape.multichip(on, freetvc::kMultichipSlot, error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                if (on) {
                    notice.show(false, "Multichip sound card in slot %d; powered on", static_cast<int>(freetvc::kMultichipSlot));
                } else {
                    notice.show_text(false, "Multichip sound card out; powered on");
                }
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), true);
                }
            } else {
                notice.set(error, true);
            }
        }
        // The card in a slot comes out: for an empty choice, or to make room for another card
        const auto take_out = [&](unsigned slot) -> bool {
            switch (machine.config().cards[slot].kind) {
            case freetvc::CardKind::Hbf: return tape.disk_card(false, error);
            case freetvc::CardKind::GameCard: return tape.gamecard(false, slot, error);
            case freetvc::CardKind::Multichip: return tape.multichip(false, slot, error);
            case freetvc::CardKind::Card256: return tape.card256(false, slot, error);
            case freetvc::CardKind::TwoDfx: return tape.twodfx(false, slot, error);
            case freetvc::CardKind::NanoSd: return tape.nanosd(false, slot, error);
            case freetvc::CardKind::Serial: return tape.serial_card(false, slot, error);
            case freetvc::CardKind::None: return true;
            }
            return true;
        };
        if (!actions.disk_rom.empty()) {
            use_disk_rom(actions.disk_rom);
        }
        if (actions.slot_clear >= 0) {
            if (take_out(static_cast<unsigned>(actions.slot_clear))) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                notice.show(false, "slot %d empty; powered on", actions.slot_clear);
            } else {
                notice.set(error, true);
            }
        }
        if (actions.slot_card != freetvc::CardKind::None && actions.slot_card_to >= 0) {
            const auto slot = static_cast<unsigned>(actions.slot_card_to);
            const int n = actions.slot_card_to;
            bool placed = false;
            const freetvc::CardKind occupant = machine.config().cards[slot].kind;
            if (occupant != freetvc::CardKind::None && occupant != actions.slot_card && !take_out(slot)) {
                actions.slot_card = freetvc::CardKind::None;
                notice.set(error, true);
            }
            switch (actions.slot_card) {
            case freetvc::CardKind::Hbf:
                if ((placed = tape.disk_card(true, error))) {
                    notice.show(false, "disk card in slot 0; powered on");
                }
                break;
            case freetvc::CardKind::GameCard:
                if ((placed = tape.gamecard(true, slot, error))) {
                    notice.show(false, "GameCard in slot %d; powered on", n);
                }
                break;
            case freetvc::CardKind::Multichip:
                if ((placed = tape.multichip(true, slot, error))) {
                    notice.show(false, "Multichip sound card in slot %d; powered on", n);
                }
                break;
            case freetvc::CardKind::Card256:
                if ((placed = tape.card256(true, slot, error))) {
                    notice.show(false, "TVC256k++ in slot %d (EXT %d); powered on", n, n);
                }
                break;
            case freetvc::CardKind::NanoSd:
                if ((placed = tape.nanosd(true, slot, error))) {
                    notice.show(false, "NanoSD in slot %d; powered on", n);
                }
                break;
            case freetvc::CardKind::Serial:
                if ((placed = tape.serial_card(true, slot, error))) {
                    notice.show(false, "serial card in slot %d; powered on", n);
                }
                break;
            case freetvc::CardKind::TwoDfx:
                if ((placed = tape.twodfx(true, slot, error))) {
                    notice.show(false, "2dfx graphics card in slot %d; powered on", n);
                }
                break;
            default:
                break;
            }
            if (placed) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                for (std::string& line : tape.take_messages()) {
                    notice.set(std::move(line), true);
                }
            } else {
                notice.set(error, true);
            }
        }
        if (actions.toggle_disk_card) {
            if (tape.disk_card(!tape.disk_card_on(), error)) {
                debugger->cancel_step();
                monitor->machine_changed();
                show_machine();
                input.machine_replaced();
                notice.show_text(false, tape.disk_card_on() ? N_("disk card in; powered on") : N_("disk card out; powered on"));
            } else {
                std::fprintf(stderr, "freetvc: %s\n", error.c_str());
                notice.show(true, "%s (the places searched are on stderr)", error.substr(0, error.find("; searched")).c_str());
            }
        }
        if (actions.disk_eject >= 0) {
            const auto drive = static_cast<unsigned>(actions.disk_eject);
            const std::string name = tape.disk_name(drive);
            if (tape.disk_eject(drive, error)) {
                notice.show(false, "%s ejected from %c:", name.c_str(), static_cast<char>('A' + drive));
            } else {
                notice.set(error, true);
            }
        }
        if (actions.disk_insert >= 0) {
            static const SDL_DialogFileFilter kDisks[] = {{N_("Disk images (*.dsk, *.img)"), "dsk;DSK;img;IMG"}, {N_("All files"), "*"}};
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = FileChoice::Purpose::Disk;
                file_choice.drive = static_cast<unsigned>(actions.disk_insert);
            }
            SDL_ShowOpenFileDialog(&FileChoice::done, &file_choice, window, file_choice.translated(kDisks, 2), 2, nullptr, false);
        }
        if (actions.cart_remove) {
            tape.cart_remove();
            debugger->cancel_step();
            monitor->machine_changed();
            show_machine();
            input.machine_replaced();
            notice.show_text(false, "cartridge removed; powered on");
        }
        if (actions.tape_run && tape.media().autostart_tape(machine)) {
            notice.show_text(false, "starting the tape");
            debugger->cancel_step();
        }
        if (actions.tape_play) {
            tape.tape_play();
        }
        if ((actions.tape_stop && !tape.tape_stop(error)) || (actions.tape_eject && !tape.tape_eject(error))) {
            notice.set(error, true);
        }
        if (actions.tape_rewind) {
            tape.tape_rewind();
        }
        if (actions.toggle_fast_disk) {
            fast_disk = !fast_disk;
            notice.show_text(false, fast_disk ? N_("fast disk: full speed while a drive works") : N_("fast disk off: the drives run at real speed"));
        }
        if (actions.toggle_fast_tape) {
            fast_tape = !fast_tape;
            notice.show_text(false, fast_tape ? N_("fast tape: full speed while the tape moves") : N_("fast tape off: the tape runs at real speed"));
        }
        if (actions.run_again && tape.media().autostart(machine)) {
            notice.show(false, "starting %s", tape.media().cassette().attached_name().c_str());
            if (debugger) {
                debugger->cancel_step();
            }
        }
        if (actions.eject) {
            tape.detach();
            notice.set(tape.describe(), false);
        }
        if (actions.mapping >= 0) {
            input.set_mapping(static_cast<freetvc::sdl::KeyMapping>(actions.mapping));
        }
        if (actions.toggle_mute && audio) {
            muted = !muted;
            SDL_SetAudioStreamGain(audio, muted ? 0.0f : static_cast<float>(volume) / 100.0f);
        }
        if (actions.toggle_fullscreen) {
            SDL_SetWindowFullscreen(window, !status.fullscreen);
        }
        if (actions.toggle_full_raster) {
            full_raster = !full_raster;
            notice.show_text(false, full_raster ? N_("full raster: sync and blanking shown, TV area outlined") : N_("TV area"));
        }
        if (actions.screenshot) {
            const auto area = full_raster ? freetvc::sdl::kFullRaster : freetvc::sdl::kTvArea;
            const std::string path = save_screenshot(machine.framebuffer(), area, screenshot_dir(), media_name(), error);
            if (path.empty()) {
                notice.show(true, "screenshot failed: %s", error.c_str());
            } else {
                notice.show(false, "saved %s", path.c_str());
            }
        }
        if (!actions.skin.empty()) {
            pending_skin = actions.skin;
        }
        if (!actions.language.empty()) {
            use_language(actions.language);
            language_chosen = true;
        }
        if (actions.reload_skin) {
            if (skins.path.empty()) {
                notice.show_text(true, "the built-in skin has no file to reload");
            } else {
                pending_skin = skins.path;
            }
        }
        if (actions.toggle_record) {
            if (recorder.recording()) {
                stop_recording();
            } else if (const auto ffmpeg = freetvc::find_ffmpeg(base_dir)) {
                std::error_code ec;
                fs::create_directories(video_dir(), ec);
                std::vector<float> before(machine.audio_available());
                machine.read_audio(before);
                const fs::path out = video_dir() / fs::path(std::u8string(reinterpret_cast<const char8_t*>(timestamped(media_name(), ".mp4").c_str())));
                if (recorder.start(out, *ffmpeg, full_raster ? freetvc::kFullRaster : freetvc::kTvArea, machine.audio_rate(), error)) {
                    machine.set_frame_sink(&recorder);
                    notice.show(false, "recording to %s", utf8(out.filename()).c_str());
                } else {
                    notice.show(true, "recording failed: %s", error.c_str());
                }
            } else {
                notice.show_text(true, "no ffmpeg for recording: the ffmpeg folder next to freeTVC is missing");
            }
        }
        if (actions.video_dir || actions.screenshot_dir) {
            {
                const std::lock_guard lock(file_choice.mutex);
                file_choice.showing = true;
                file_choice.purpose = actions.video_dir ? FileChoice::Purpose::VideoDir : FileChoice::Purpose::ShotDir;
            }
            const std::string start = utf8(actions.video_dir ? video_dir() : screenshot_dir());
            SDL_ShowOpenFolderDialog(&FileChoice::done, &file_choice, window, start.c_str(), false);
        }
        if (actions.open_video_dir) {
            open_folder(video_dir());
        }
        if (actions.open_screenshot_dir) {
            open_folder(screenshot_dir());
        }
        running &= !actions.quit;

        // freetvc.ini follows the state: every frame while the sheet is open, else once a second
        if (settings_view.is_open() || now_ns - settings_checked_ns >= 1'000'000'000ULL) {
            settings_checked_ns = now_ns;
            if (freetvc::SettingValues values = collect_settings(); values != saved_settings) {
                save_settings(values);
                saved_settings = std::move(values);
            }
        }

        if (frame_limit > 0 && ++frames >= frame_limit) {
            running = false;
        }
    }

    if (const freetvc::SettingValues values = collect_settings(); values != saved_settings) {
        save_settings(values);
    }
    if (recorder.recording()) {
        stop_recording();
    }

    if (audio) {
        SDL_DestroyAudioStream(audio);
    }
    display->destroy_texture(vram_texture);
    display->destroy_texture(texture);
    if (splash != ImTextureID_Invalid) {
        display->destroy_texture(splash);
    }
    about_view.release(*display);
    display.reset();  // its ImGui backends and the window go with it
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}
