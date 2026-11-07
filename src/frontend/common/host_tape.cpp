// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/host_tape.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "core/cards/card256.hpp"
#include "core/cards/gamecard.hpp"
#include "core/cards/multichip.hpp"
#include "core/cards/twodfx_card.hpp"
#include "core/cards/nanosd.hpp"
#include "core/cards/serial_card.hpp"
#include "core/media/tape_signal.hpp"
#include "core/message.hpp"
#include "core/snapshot/machine_state.hpp"
#include "core/snapshot/tvs_import.hpp"
#include "frontend/common/game_saves.hpp"
#include "remote/png.hpp"

#include "frontend/common/multicart_image.hpp"
#include "frontend/common/presets.hpp"

namespace fs = std::filesystem;

namespace freetvc {

namespace {

constexpr std::size_t kMaxCasSize = 1 << 20;
constexpr std::size_t kMaxTapeSize = std::size_t{1} << 30;  // a WAV of an hour at 44.1 kHz, 16-bit stereo, is 635 MB
constexpr std::size_t kPage = 0x4000;
constexpr std::size_t kMaxCartSize = 64 * kPage;  // MegaCart
constexpr std::size_t kMaxSnapshotSize = std::size_t{16} << 20;

bool same_ascii(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

std::string minutes(std::uint64_t ticks) {
    const std::uint64_t seconds = ticks / kTapeTicksPerSecond;
    char text[16];
    std::snprintf(text, sizeof text, "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
                  static_cast<unsigned long long>(seconds % 60));
    return text;
}

// A tape file as a signal: WAV as recorded, TTP and CAS encoded with the ROM's format (design/media.md "Tape deck")
std::optional<TapeSignal> load_tape(const fs::path& path, std::string& error) {
    const std::string ext = path.extension().string();
    const auto bytes = read_file(path.string(), same_ascii(ext, ".wav") ? kMaxTapeSize : kMaxCasSize, error);
    if (!bytes) {
        return std::nullopt;
    }
    const std::string name = tvc_file_name(path.stem().string());
    if (same_ascii(ext, ".wav")) {
        auto signal = wav_signal(*bytes, error);
        if (signal) {
            signal->name = name;
        }
        return signal;
    }
    if (same_ascii(ext, ".ttp")) {
        const auto programs = parse_ttp(*bytes, error);
        if (!programs) {
            return std::nullopt;
        }
        return encode_tape(*programs);
    }
    if (same_ascii(ext, ".cas")) {
        const auto cas = parse_cas(*bytes, error);
        if (!cas) {
            return std::nullopt;
        }
        const std::vector<TapeProgram> programs{cas_program(*cas, name)};
        return encode_tape(programs);
    }
    error = msg("a tape is a .wav, .ttp or .cas file");
    return std::nullopt;
}

// The build date the SD Dual's firmware keeps at C020h ("2022.11.09."), or "?"
std::string sddual_date(std::span<const std::uint8_t> firmware) {
    std::string date;
    for (std::size_t i = 0x20; i < std::min<std::size_t>(firmware.size(), 0x2A); ++i) {
        const auto c = static_cast<char>(firmware[i]);
        if ((c < '0' || c > '9') && c != '.') {
            return "?";
        }
        date += c;
    }
    return date.size() == 10 ? date : "?";
}

}  // namespace

std::optional<std::vector<std::uint8_t>> CasFolder::read(std::string_view name, std::string& found) {
    if (name.empty()) {
        return std::nullopt;
    }
    const bool has_extension = name.find('.') != std::string_view::npos;
    std::error_code ec;
    std::vector<fs::path> matches;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path& p = entry.path();
        if (has_extension ? same_ascii(p.filename().string(), name)
                          : same_ascii(p.extension().string(), ".cas") && same_ascii(p.stem().string(), name)) {
            matches.push_back(p);
        }
    }
    if (matches.empty()) {
        return std::nullopt;
    }
    std::sort(matches.begin(), matches.end());  // the same pick every time when only the case differs
    std::string error;
    auto bytes = read_file(matches.front().string(), kMaxCasSize, error);
    if (bytes) {
        found = has_extension ? matches.front().filename().string() : matches.front().stem().string();
    }
    return bytes;
}

bool CasFolder::write(std::string_view name, std::span<const std::uint8_t> cas, std::string& error) {
    if (!writable_) {
        error = msg("the cassette folder is read only");
        return false;
    }
    std::string file(name);
    for (char& c : file) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b > 0x7E || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) {
            c = '_';
        }
    }
    if (file.empty() || file.find_first_not_of('.') == std::string::npos) {
        error = msg("no usable file name");
        return false;
    }
    if (file.find('.') == std::string::npos) {
        file += ".CAS";
    }
    const fs::path path = dir_ / file;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(cas.data()), static_cast<std::streamsize>(cas.size()));
    if (!out) {
        error = msg("cannot write %s", path.string().c_str());
        return false;
    }
    return true;
}

HostTape::HostTape(Machine& machine) : machine_(machine) {
    machine_.set_os_call_handler(&media_);
    machine_.set_tape_port(&media_.deck());
    machine_.set_floppy_drives(&drives_);
    machine_.set_card256_media(&card256_host_.media());
    machine_.set_nanosd_media(&nanosd_media_);
    machine_.set_multicart_flash(&multicart_flash_);
    machine_.set_serial_line(&serial_);
}

HostTape::~HostTape() {
    std::string error;
    if (!save_recording(error)) {
        std::fprintf(stderr, "freetvc: %s\n", error.c_str());
    }
    const bool disks_saved = save_disks();
    if (!save_multicart() || !disks_saved) {
        for (const std::string& line : messages_) {
            std::fprintf(stderr, "freetvc: %s\n", line.c_str());
        }
    }
    machine_.set_multicart_flash(nullptr);
    machine_.set_sd_image(0, nullptr);
    machine_.set_sd_image(1, nullptr);
    keep_prompt_state();
    machine_.set_os_call_handler(nullptr);
    machine_.set_tape_port(nullptr);
    machine_.set_floppy_drives(nullptr);
    machine_.set_card256_media(nullptr);
    machine_.set_nanosd_media(nullptr);
    machine_.set_printer(nullptr);
    machine_.set_serial_line(nullptr);
}

bool HostTape::set_folder(const std::string& dir, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        error = msg("'%s' is not a directory", dir.c_str());
        return false;
    }
    folder_.emplace(dir, true);
    folder_given_ = true;
    media_.cassette().set_folder(&*folder_);
    return true;
}

bool HostTape::attach(const std::string& path, std::string& error) {
    if (is_tape_file(path)) {
        return tape_insert(path, error);
    }
    const auto bytes = read_file(path, kMaxCasSize, error);
    if (!bytes) {
        return false;
    }
    std::string parse_error;
    auto file = parse_cas(*bytes, parse_error);
    if (!file) {
        error = msg("%s: %s", path.c_str(), parse_error.c_str());
        return false;
    }
    const fs::path p(path);
    for (const std::string& w : file->warnings) {
        messages_.push_back(msg("%s: %s", p.filename().string().c_str(), w.c_str()));
    }
    media_.cassette().attach(p.stem().string(), std::move(*file));
    attached_path_ = p;
    if (media_.deck().loaded()) {
        // While a tape is in, the host cassette serves nothing: a CAS for the host cassette takes the tape out
        std::string eject_error;
        if (!tape_eject(eject_error)) {
            messages_.push_back(eject_error);
        }
        messages_.push_back(msg("tape ejected for the cassette %s", media_.cassette().attached_name().c_str()));
    }
    if (!folder_given_) {
        folder_.emplace(p.has_parent_path() ? p.parent_path() : fs::path("."), false);
        media_.cassette().set_folder(&*folder_);
    }
    return true;
}

void HostTape::detach() {
    media_.cassette().detach();
    attached_path_.clear();
    if (!folder_given_) {
        media_.cassette().set_folder(nullptr);
        folder_.reset();
    }
}

bool HostTape::autostart(const std::string& path, std::string& error) {
    if (is_tape_file(path)) {
        return tape_autostart(path, error);
    }
    if (is_disk_file(path)) {
        return disk_autostart(path, error);
    }
    if (!attach(path, error)) {
        return false;
    }
    program_name_ = fs::path(path).stem().string();
    keep_prompt_state();
    fetch_prompt_state();
    nanosd_media_.skip_menu = nanosd_slot() >= 0;
    media_.autostart(machine_);
    return true;
}

bool HostTape::is_tape_file(const std::string& path) {
    const std::string ext = fs::path(path).extension().string();
    return same_ascii(ext, ".wav") || same_ascii(ext, ".ttp");
}

bool HostTape::tape_insert(const std::string& path, std::string& error) {
    auto signal = load_tape(path, error);
    if (!signal) {
        error = msg("%s: %s", fs::path(path).filename().string().c_str(), error.c_str());
        return false;
    }
    if (!tape_eject(error)) {
        return false;
    }
    if (media_.cassette().attached()) {
        messages_.push_back(msg("cassette %s detached for the tape", media_.cassette().attached_name().c_str()));
        detach();
    }
    media_.deck().insert(std::move(*signal));
    tape_path_ = path;
    messages_.push_back(msg("tape %s in (%s)", fs::path(path).filename().string().c_str(), minutes(media_.deck().length()).c_str()));
    return true;
}

bool HostTape::tape_autostart(const std::string& path, std::string& error) {
    if (!tape_insert(path, error)) {
        return false;
    }
    program_name_ = fs::path(path).stem().string();
    keep_prompt_state();
    fetch_prompt_state();
    nanosd_media_.skip_menu = nanosd_slot() >= 0;
    media_.autostart_tape(machine_);
    return true;
}

bool HostTape::tape_record(const std::string& path, std::string& error) {
    if (!tape_eject(error)) {
        return false;
    }
    media_.deck().insert_blank();
    media_.deck().record();
    record_path_ = path;
    tape_path_.clear();
    messages_.push_back(msg("recording to %s", path.c_str()));
    return true;
}

void HostTape::tape_play() { media_.deck().play(); }

bool HostTape::tape_stop(std::string& error) {
    media_.deck().stop();
    return save_recording(error);
}

void HostTape::tape_rewind() { media_.deck().rewind(); }

bool HostTape::tape_eject(std::string& error) {
    const bool saved = tape_stop(error);
    media_.deck().eject();
    tape_path_.clear();
    return saved;
}

bool HostTape::save_recording(std::string& error) {
    if (record_path_.empty()) {
        return true;
    }
    const fs::path path = record_path_;
    record_path_.clear();
    const std::vector<std::uint8_t> wav = signal_wav(media_.deck().signal());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    if (!out) {
        error = msg("cannot write %s", path.string().c_str());
        return false;
    }
    tape_path_ = path;
    messages_.push_back(msg("recording saved: %s (%s)", path.string().c_str(), minutes(media_.deck().length()).c_str()));
    return true;
}

std::string HostTape::tape_describe() const {
    const TapeDeck& deck = media_.deck();
    if (!deck.loaded()) {
        return msg("no tape");
    }
    const std::string state = deck.state() == TapeState::Playing ? msg("playing") : deck.state() == TapeState::Recording ? msg("recording") : msg("stopped");
    const std::string file = !record_path_.empty() ? record_path_.filename().string() : tape_path_.filename().string();
    return msg(deck.motor() ? N_("tape %s, %s, %s / %s, motor on") : N_("tape %s, %s, %s / %s, motor off"),
               (file.empty() ? msg("(blank)") : file).c_str(), state.c_str(), minutes(deck.position()).c_str(), minutes(deck.length()).c_str());
}

std::optional<CartType> parse_cart_type(std::string_view name) {
    for (const CartType type : {CartType::Plain, CartType::KiloCart, CartType::MegaCart, CartType::MultiCart, CartType::SdDual}) {
        if (same_ascii(name, cart_type_name(type))) {
            return type;
        }
    }
    return std::nullopt;
}

const char* cart_type_name(CartType type) {
    switch (type) {
    case CartType::KiloCart:
        return "kilocart";
    case CartType::MegaCart:
        return "megacart";
    case CartType::MultiCart:
        return "multicart";
    case CartType::SdDual:
        return "sddual";
    default:
        return "plain";
    }
}

bool load_cart(const std::string& path, const std::string& type, unsigned bank, RomImages& roms, std::string& error,
               std::vector<std::uint8_t>* flash) {
    auto bytes = read_file(path, kMaxCartSize, error);
    if (!bytes) {
        return false;
    }
    if (bytes->empty()) {
        error = msg("%s: empty file", path.c_str());
        return false;
    }
    CartType chosen = is_multicart_image(*bytes)   ? CartType::MultiCart
                      : is_sddual_firmware(*bytes) ? CartType::SdDual
                      : bytes->size() <= kPage     ? CartType::Plain
                      : bytes->size() <= 4 * kPage ? CartType::KiloCart
                                                   : CartType::MegaCart;
    if (!type.empty()) {
        const auto named = parse_cart_type(type);
        if (!named) {
            error = msg("unknown cartridge type '%s' (plain, kilocart, megacart, multicart, sddual)", type.c_str());
            return false;
        }
        chosen = *named;
    }
    if (chosen == CartType::MultiCart) {
        if (!flash) {
            error = msg("%s: a MultiCart image needs the host's flash", path.c_str());
            return false;
        }
        // A shorter file fills chip 0 first; the rest is erased flash
        bytes->resize(MultiCartFlash::kSize, 0xFF);
        *flash = std::move(*bytes);
        roms.cart.clear();
        roms.cart_type = CartType::MultiCart;
        roms.cart_bank = 0;
        return true;
    }
    if (chosen == CartType::SdDual && bytes->size() > SdDual::kFlashSize) {
        error = msg("%s: %zu bytes is larger than the SD Dual's 64 KiB flash", path.c_str(), bytes->size());
        return false;
    }
    const std::size_t limit = chosen == CartType::MegaCart ? kMaxCartSize : chosen == CartType::KiloCart ? 4 * kPage : 0;
    if (limit != 0 && bytes->size() > limit) {
        error = msg("%s: %zu bytes is too large for a %s", path.c_str(), bytes->size(), cart_type_name(chosen));
        return false;
    }
    const std::size_t pages = (bytes->size() + kPage - 1) / kPage;
    if (chosen == CartType::Plain && bank >= pages) {
        error = msg("%s: bank %u is past the image (%zu banks)", path.c_str(), bank, pages);
        return false;
    }
    roms.cart = std::move(*bytes);
    roms.cart_type = chosen;
    roms.cart_bank = chosen == CartType::Plain ? bank : 0;
    return true;
}

bool HostTape::is_cart_file(const std::string& path) {
    const std::string ext = fs::path(path).extension().string();
    return same_ascii(ext, ".crt") || same_ascii(ext, ".rom") || same_ascii(ext, ".bin");
}

bool HostTape::cart_insert(const std::string& path, const std::string& type, unsigned bank, std::string& error) {
    // The flash in the machine goes back to its file first: the same file may be going in again
    save_multicart();
    MachineConfig config = machine_.config();
    std::vector<std::uint8_t> flash;
    if (!load_cart(path, type, bank, config.roms, error, &flash)) {
        return false;
    }
    drop_multicart();
    if (config.roms.cart_type == CartType::SdDual) {
        config.roms.cart_bank = sddual_writable_ ? 1 : 0;
    }
    media_.cancel();
    machine_.configure(config);
    cart_path_ = path;
    program_name_ = fs::path(path).stem().string();
    if (config.roms.cart_type == CartType::MultiCart) {
        set_multicart(path, std::move(flash));
    }
    return true;
}

void HostTape::set_multicart(const std::string& path, std::vector<std::uint8_t> flash) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    multicart_file_size_ = ec ? flash.size() : std::min<std::size_t>(static_cast<std::size_t>(size), flash.size());
    multicart_flash_.bytes = std::move(flash);
    multicart_flash_.dirty = false;
    multicart_flash_.last_write = 0;
    cart_path_ = path;
}

void HostTape::drop_multicart() {
    std::string error;
    if (!save_multicart(error)) {
        messages_.push_back(error);
    }
    multicart_flash_ = {};
    multicart_file_size_ = 0;
}

bool HostTape::save_multicart(std::string& error) {
    if (!multicart_flash_.dirty || multicart_flash_.bytes.empty()) {
        return true;
    }
    multicart_flash_.dirty = false;
    // The file keeps its length while the flash past it is still erased (a one-chip image stays one chip)
    const std::vector<std::uint8_t>& bytes = multicart_flash_.bytes;
    const bool tail_erased = std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(multicart_file_size_), bytes.end(),
                                         [](std::uint8_t b) { return b == 0xFF; });
    const std::size_t size = tail_erased ? multicart_file_size_ : bytes.size();
    const fs::path temp = cart_path_.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!out) {
            error = msg("cannot write the MultiCart's flash to %s", temp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, cart_path_, ec);
    if (ec) {
        error = msg("cannot write the MultiCart's flash to %s: %s", cart_path_.string().c_str(), ec.message().c_str());
        return false;
    }
    multicart_file_size_ = size;
    return true;
}

bool HostTape::save_multicart() {
    std::string error;
    if (!save_multicart(error)) {
        messages_.push_back(error);
        return false;
    }
    return true;
}

bool HostTape::multicart_build(const std::string& out, const std::vector<std::string>& files, const std::string& language,
                               std::string& error) {
    if (fs::path(out) == cart_path_ && !multicart_flash_.bytes.empty()) {
        error = msg("%s is the MultiCart in the machine", out.c_str());
        return false;
    }
    return write_multicart_image(out, files, language, rom_places_, error);
}

void HostTape::cart_remove() {
    drop_multicart();
    MachineConfig config = machine_.config();
    config.roms.cart.clear();
    config.roms.cart_type = CartType::Plain;
    config.roms.cart_bank = 0;
    media_.cancel();
    machine_.configure(config);
    if (program_name_ == cart_path_.stem().string()) {
        program_name_.clear();
    }
    cart_path_.clear();
}

std::string HostTape::cart_describe() const {
    const RomImages& roms = machine_.config().roms;
    if (!roms.has_cart()) {
        return msg("no cartridge");
    }
    const MemoryMap& memory = machine_.memory();
    const std::string name = cart_path_.empty() ? msg("image") : cart_path_.filename().string();
    if (const MultiCart* mc = memory.multicart()) {
        const std::uint8_t r = mc->reg();
        const std::string reg = msg("register %02Xh (%s page %u%s), %s", r, (r & MultiCart::kRam) ? "RAM" : (r & MultiCart::kChip1) ? "flash 1" : "flash 0",
                                    (r & MultiCart::kRam) ? (r & 7u) : (r & 0x1Fu), (r & MultiCart::kRelocated) ? msg(", at E000h").c_str() : "",
                                    mc->reset_on_read() ? msg("a read of C000h resets it").c_str() : msg("no reset on read").c_str());
        const std::size_t files = multicart_directory(multicart_flash_.bytes).size();
        return msg(files == 1 ? N_("cartridge %s: multicart, %s, %zu file%s") : N_("cartridge %s: multicart, %s, %zu files%s"), name.c_str(), reg.c_str(),
                   files, multicart_flash_.dirty ? msg(", flash changed (not yet saved)").c_str() : "");
    }
    if (const SdDual* sd = memory.sddual()) {
        return msg("cartridge %s: sddual, firmware %s, flash page %u%s", name.c_str(), sddual_date(roms.cart).c_str(), static_cast<unsigned>(sd->page()),
                   sd->writable() ? msg(", flash write jumper on").c_str() : "");
    }
    return msg("cartridge %s: %s, %zu bytes, page %u of %u", name.c_str(), cart_type_name(roms.cart_type), roms.cart.size(), memory.cart_page(),
               memory.cart_pages());
}

fs::path HostTape::prompt_file(const MachineConfig& c) const {
    char name[96];
    std::snprintf(name, sizeof name, "prompt-%u-%08X-%08X-%08X-%u-%u%s", static_cast<unsigned>(c.model),
                  static_cast<unsigned>(crc32(c.roms.sys)), static_cast<unsigned>(crc32(c.roms.ext)),
                  static_cast<unsigned>(crc32(c.roms.cart)), static_cast<unsigned>(c.roms.cart_type), c.roms.cart_bank,
                  c.colour ? "" : "-bw");
    std::string file = name;
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (c.cards[i].kind != CardKind::None) {
            char card[32];
            std::snprintf(card, sizeof card, "-s%u%s%08X", i, card_kind_name(c.cards[i].kind), static_cast<unsigned>(crc32(c.cards[i].rom)));
            file += card;
        }
    }
    return prompt_dir_ / (file + ".tvcs");
}

void HostTape::fetch_prompt_state() {
    if (prompt_dir_.empty() || media_.has_prompt_state()) {
        return;
    }
    std::string error;
    const fs::path file = prompt_file(machine_.config());
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return;
    }
    const auto bytes = read_file(file.string(), kMaxSnapshotSize, error);
    auto snapshot = bytes ? Snapshot::decode(*bytes, error) : std::nullopt;
    Machine state(machine_.config());
    if (!snapshot || !state.load_state(*snapshot, error)) {
        // An old or damaged file: a new one replaces it after this autostart
        fs::remove(file, ec);
        return;
    }
    media_.set_prompt_state(state);
}

void HostTape::keep_prompt_state() {
    const Machine* state = media_.prompt_state();
    if (prompt_dir_.empty() || !state) {
        return;
    }
    std::error_code ec;
    const fs::path file = prompt_file(state->config());
    if (fs::exists(file, ec)) {
        return;
    }
    fs::create_directories(prompt_dir_, ec);
    Snapshot snapshot;
    state->save_state(snapshot);
    const std::vector<std::uint8_t> bytes = snapshot.encode();
    const fs::path temp = file.string() + ".tmp";
    std::ofstream(temp, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()),
                                                                  static_cast<std::streamsize>(bytes.size()));
    fs::rename(temp, file, ec);
}

std::vector<std::uint8_t> HostTape::snapshot_bytes() {
    Snapshot snapshot;
    machine_.save_state(snapshot);
    // MEDI: what the files around the machine were (tape and folder by path)
    ChunkWriter& w = snapshot.add("MEDI");
    const TapeDeck& deck = media_.deck();
    const bool tape = deck.loaded() && deck.state() != TapeState::Recording && !tape_path_.empty();
    w.text(tape ? tape_path_.string() : std::string());
    w.u8(static_cast<std::uint8_t>(deck.state()));
    w.boolean(deck.motor());
    w.u64(deck.position());
    w.u64(deck.last_tick());
    w.text(media_.cassette().attached() ? attached_path_.string() : std::string());
    w.text(folder_given_ && folder_ ? folder_->dir().string() : std::string());
    w.text(cart_path_.string());
    for (const fs::path& disk : disk_paths_) {
        w.text(disk.string());
    }
    for (const SdImageFile& sd : sd_files_) {
        w.text(sd.is_open() ? sd.path().string() : std::string());
    }
    // The cards' own media only while the card is in
    const bool card256_in = card256_slot() >= 0;
    w.text(nanosd_slot() >= 0 ? nanosd_sd_dir().string() : std::string());
    w.u8(static_cast<std::uint8_t>(card256_in ? card256_host_.usb() : Card256Usb::None));
    w.text(card256_in ? card256_host_.pendrive_dir().string() : std::string());
    snapshot.add("GAME").text(game_name());
    return snapshot.encode();
}

namespace {

// The MEDI chunk; fields an older snapshot does not have are marked as missing
struct MediaRecord {
    std::string tape;
    TapeState state = TapeState::Stopped;
    bool motor = false;
    std::uint64_t position = 0;
    std::uint64_t last_tick = 0;
    std::string cassette;
    std::string folder;
    std::string cart;
    bool has_disks = false;
    std::array<std::string, FloppyDrives::kDrives> disks;
    bool has_sds = false;
    std::array<std::string, 2> sds;
    bool has_cards = false;
    std::string nanosd;
    Card256Usb usb = Card256Usb::None;
    std::string pendrive;
};

MediaRecord read_media(const SnapshotChunk& medi) {
    ChunkReader r(medi.body.data);
    MediaRecord m;
    m.tape = r.text();
    m.state = static_cast<TapeState>(std::min<unsigned>(r.u8(), 3));
    m.motor = r.boolean();
    m.position = r.u64();
    m.last_tick = r.u64();
    m.cassette = r.text();
    m.folder = r.text();
    m.cart = r.text();
    for (std::string& disk : m.disks) {
        disk = r.text();
    }
    m.has_disks = !r.failed();
    for (std::string& sd : m.sds) {
        sd = r.text();
    }
    m.has_sds = !r.failed();
    m.nanosd = r.text();
    m.usb = static_cast<Card256Usb>(std::min<unsigned>(r.u8(), 2));
    m.pendrive = r.text();
    m.has_cards = !r.failed();
    return m;
}

}  // namespace

std::optional<std::vector<SnapshotPath>> HostTape::snapshot_paths(std::span<const std::uint8_t> bytes, std::string& error) {
    auto snapshot = Snapshot::decode(bytes, error);
    if (!snapshot) {
        return std::nullopt;
    }
    std::vector<SnapshotPath> paths;
    const SnapshotChunk* medi = snapshot->find("MEDI");
    if (!medi) {
        return paths;
    }
    const MediaRecord m = read_media(*medi);
    const auto add = [&](std::string kind, const std::string& path, bool directory) {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        const bool found = directory ? fs::is_directory(path, ec) : fs::is_regular_file(path, ec);
        paths.push_back({std::move(kind), path, directory, found});
    };
    add("tape", m.tape, false);
    add("cassette", m.cassette, false);
    add("folder", m.folder, true);
    add("cartridge", m.cart, false);
    for (unsigned d = 0; m.has_disks && d < m.disks.size(); ++d) {
        add(std::string("disk ") + static_cast<char>('A' + d) + ":", m.disks[d], false);
    }
    for (unsigned i = 0; m.has_sds && i < m.sds.size(); ++i) {
        add("SD " + std::to_string(i + 1), m.sds[i], false);
    }
    if (m.has_cards) {
        add("NanoSD", m.nanosd, true);
        add("pendrive", m.usb == Card256Usb::Pendrive ? m.pendrive : std::string(), true);
    }
    return paths;
}

bool HostTape::snapshot_load_moved(const std::string& path, const SnapshotPathMap& moved, std::string& error) {
    const auto bytes = read_file(path, kMaxSnapshotSize, error);
    return bytes && snapshot_load_bytes(*bytes, fs::path(path).filename().string(), error, &moved);
}

std::optional<std::vector<SnapshotPath>> HostTape::snapshot_check(const std::string& path, std::string& error) {
    const auto bytes = read_file(path, kMaxSnapshotSize, error);
    if (!bytes) {
        return std::nullopt;
    }
    auto paths = snapshot_paths(*bytes, error);
    if (!paths) {
        error = msg("%s: %s", fs::path(path).filename().string().c_str(), error.c_str());
    }
    return paths;
}

std::string HostTape::game_root() const { return save_root_.string(); }

std::vector<std::string> HostTape::game_list() const { return save_root_.empty() ? std::vector<std::string>{} : list_games(save_root_); }

std::vector<GameSaveEntry> HostTape::game_saves(const std::string& game) const {
    std::vector<GameSaveEntry> entries;
    if (!save_root_.empty()) {
        for (const GameSave& save : list_saves(save_root_, game)) {
            entries.push_back({save.file.string(), save.picture.string(), save.name, save.when});
        }
    }
    return entries;
}

std::string HostTape::game_save_file(const std::string& game, const std::string& name) const {
    const std::string folder = game.empty() ? game_name() : game;
    return named_save_file(save_root_, folder.empty() ? std::string("TVC") : folder, name).string();
}

bool HostTape::game_save(const std::string& game, const std::string& save_name, std::string& file, std::string& error) {
    if (save_root_.empty()) {
        error = msg("game saves are off in this front end");
        return false;
    }
    std::string name = game.empty() ? game_name() : game;
    if (name.empty()) {
        name = "TVC";
    }
    fs::path target;
    if (save_name.empty()) {
        target = new_save_file(save_root_, name, error);
    } else {
        target = named_save_file(save_root_, name, save_name);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = msg("cannot make %s: %s", target.parent_path().string().c_str(), ec.message().c_str());
            target.clear();
        }
    }
    if (target.empty()) {
        return false;
    }
    const std::string kept = program_name_;
    program_name_ = name;
    const bool saved = snapshot_save(target.string(), error);
    if (!saved) {
        program_name_ = kept;
        return false;
    }
    std::string ignored;
    remote::write_png(fs::path(target).replace_extension(".png").string(), machine_.framebuffer(), kTvArea, ignored);
    file = target.string();
    return true;
}

bool HostTape::game_save_delete(const std::string& file, std::string& error) {
    std::error_code ec;
    const fs::path target = fs::weakly_canonical(file, ec);
    const fs::path root = fs::weakly_canonical(save_root_, ec);
    const auto [end, ignored] = std::mismatch(root.begin(), root.end(), target.begin(), target.end());
    if (save_root_.empty() || end != root.end() || target.extension() != ".tvcs") {
        error = msg("%s is not a game save", file.c_str());
        return false;
    }
    const fs::path picture = fs::path(target).replace_extension(".png");
    return delete_save({target, fs::exists(picture, ec) ? picture : fs::path(), {}, {}}, error);
}

bool HostTape::snapshot_save(const std::string& path, std::string& error) {
    const std::vector<std::uint8_t> bytes = snapshot_bytes();
    const fs::path target(path);
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = msg("cannot write %s", temp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        error = msg("cannot write %s: %s", path.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool HostTape::snapshot_load(const std::string& path, std::string& error) {
    const auto bytes = read_file(path, kMaxSnapshotSize, error);
    if (!bytes) {
        return false;
    }
    if (same_ascii(fs::path(path).extension().string(), ".tvs")) {
        // WinTVC's format: into this board and ROM set; its media are not recorded
        media_.cancel();
        if (!import_tvs(machine_, *bytes, error)) {
            error = msg("%s: %s", fs::path(path).filename().string().c_str(), error.c_str());
            return false;
        }
        return true;
    }
    return snapshot_load_bytes(*bytes, fs::path(path).filename().string(), error);
}

bool HostTape::snapshot_load_bytes(std::span<const std::uint8_t> bytes, const std::string& from, std::string& error,
                                   const SnapshotPathMap* moved) {
    auto snapshot = Snapshot::decode(bytes, error);
    const auto needs = snapshot ? snapshot_needs(*snapshot, error) : std::nullopt;
    if (!needs) {
        error = msg("%s: %s", from.c_str(), error.c_str());
        return false;
    }
    // The machine's own ROMs first, then every preset of that board from the ROM folders, matched by CRC-32
    std::optional<MachineConfig> config;
    if (MachineConfig own = apply_needs(machine_.config(), *needs); snapshot_matches(own, *needs)) {
        config = std::move(own);
    }
    for (std::size_t i = 0; !config && i < kPresets.size(); ++i) {
        if (kPresets[i].model != needs->model || rom_places_.empty()) {
            continue;
        }
        std::string ignored;
        if (auto found = find_preset_roms(kPresets[i], rom_places_, ignored)) {
            MachineConfig candidate = apply_needs(std::move(*found), *needs);
            candidate.audio_rate = machine_.config().audio_rate;
            if (snapshot_matches(candidate, *needs)) {
                config = std::move(candidate);
            }
        }
    }
    if (!config) {
        error = msg("%s: no ROM set with SYS %08X, EXT %08X for that board was found", from.c_str(), static_cast<unsigned>(needs->sys_crc),
                    static_cast<unsigned>(needs->ext_crc));
        return false;
    }
    media_.cancel();
    if (!snapshot_matches(machine_.config(), *needs)) {
        machine_.configure(*config);
    }
    if (!machine_.load_state(*snapshot, error)) {
        error = msg("%s: %s", from.c_str(), error.c_str());
        return false;
    }

    if (const SnapshotChunk* game = snapshot->find("GAME")) {
        ChunkReader r(game->body.data);
        if (std::string name = r.text(); !r.failed() && !name.empty()) {
            program_name_ = std::move(name);
        }
    }
    // MEDI: files that are gone are left out with a message
    if (const SnapshotChunk* medi = snapshot->find("MEDI")) {
        MediaRecord m = read_media(*medi);
        // A moved folder takes the files under it along
        const auto where = [&](std::string& path) {
            if (!moved || path.empty()) {
                return;
            }
            if (const auto it = moved->find(path); it != moved->end()) {
                path = it->second;
                return;
            }
            for (const auto& [old_path, new_path] : *moved) {
                fs::path old_dir = fs::path(old_path).lexically_normal();
                if (old_dir.filename().empty()) {
                    old_dir = old_dir.parent_path();
                }
                const fs::path file = fs::path(path).lexically_normal();
                const auto [end, ignored] = std::mismatch(old_dir.begin(), old_dir.end(), file.begin(), file.end());
                if (!old_path.empty() && end == old_dir.end()) {
                    path = (fs::path(new_path) / file.lexically_relative(old_dir)).lexically_normal().string();
                    return;
                }
            }
        };
        for (std::string* path : {&m.tape, &m.cassette, &m.folder, &m.cart, &m.nanosd, &m.pendrive}) {
            where(*path);
        }
        for (std::string& path : m.disks) {
            where(path);
        }
        for (std::string& path : m.sds) {
            where(path);
        }
        const std::string& tape = m.tape;
        const std::string& cassette = m.cassette;
        const std::string& folder = m.folder;
        const fs::path cart = m.cart;
        // A MultiCart's flash by path, like a disk: the same image stays as it is, flash writes included
        if (machine_.config().roms.cart_type == CartType::MultiCart) {
            if (cart != cart_path_ || multicart_flash_.bytes.empty()) {
                drop_multicart();
                RomImages ignored;
                std::vector<std::uint8_t> flash;
                std::string why = msg("none named");
                if (!cart.empty() && load_cart(cart.string(), "multicart", 0, ignored, why, &flash)) {
                    set_multicart(cart.string(), std::move(flash));
                } else {
                    messages_.push_back(msg("snapshot: MultiCart image left out: %s", why.c_str()));
                }
            }
        } else if (!multicart_flash_.bytes.empty()) {
            drop_multicart();
        }
        cart_path_ = cart;
        const auto& disks = m.disks;
        std::string why;
        // Disks by path; a snapshot from before the disk card names none, and its drives stay as they are
        if (m.has_disks) {
            for (unsigned d = 0; d < FloppyDrives::kDrives; ++d) {
                if (disks[d].empty()) {
                    if (drives_.disk(d) && !disk_eject(d, why)) {
                        messages_.push_back(msg("snapshot: %s", why.c_str()));
                    }
                } else if (disk_paths_[d] != fs::path(disks[d]) || !drives_.disk(d)) {
                    if (!insert_disk_file(d, disks[d], why)) {
                        messages_.push_back(msg("snapshot: disk left out: %s", why.c_str()));
                    }
                }
            }
        }
        // SD card images by path; older snapshots name none, and the sockets stay as they are
        const auto& sds = m.sds;
        if (m.has_sds) {
            for (unsigned s = 0; s < sds.size(); ++s) {
                if (sds[s].empty()) {
                    sd_eject(s);
                } else if (sd_files_[s].path() != fs::path(sds[s]) || !sd_files_[s].is_open()) {
                    if (!sd_insert(s, sds[s], why)) {
                        messages_.push_back(msg("snapshot: SD card left out: %s", why.c_str()));
                    }
                }
            }
        }
        if (tape.empty()) {
            tape_eject(why);
        } else if (tape_path_ != fs::path(tape) || !media_.deck().loaded()) {
            if (!tape_insert(tape, why)) {
                messages_.push_back(msg("snapshot: tape left out: %s", why.c_str()));
            }
        }
        if (!tape.empty() && media_.deck().loaded()) {
            media_.deck().restore(m.state, m.motor, m.position, m.last_tick);
        }
        // The NanoSD's folder and the TVC256k++'s USB port by path; older snapshots name none, and they stay as they are
        if (m.has_cards && nanosd_slot() >= 0) {
            std::error_code ec;
            if (m.nanosd.empty() || !fs::is_directory(m.nanosd, ec)) {
                if (nanosd_folder_) {
                    nanosd_folder_.reset();
                    nanosd_media_.sd = nullptr;
                }
                if (!m.nanosd.empty()) {
                    messages_.push_back(msg("snapshot: NanoSD folder left out: %s", m.nanosd.c_str()));
                }
            } else if (nanosd_sd_dir() != fs::path(m.nanosd)) {
                nanosd_media_.sd = nullptr;
                nanosd_folder_.emplace(fs::absolute(m.nanosd, ec).lexically_normal());
                nanosd_media_.sd = &*nanosd_folder_;
            }
        }
        if (m.has_cards && card256_slot() >= 0) {
            std::error_code ec;
            if (m.usb == Card256Usb::Mouse) {
                card256_host_.plug_mouse();
            } else if (m.usb == Card256Usb::Pendrive && !m.pendrive.empty() && fs::is_directory(m.pendrive, ec)) {
                if (card256_host_.pendrive_dir() != fs::path(m.pendrive) && !card256_host_.plug_pendrive(m.pendrive, why)) {
                    messages_.push_back(msg("snapshot: pendrive left out: %s", why.c_str()));
                }
            } else {
                card256_host_.unplug();
                if (m.usb == Card256Usb::Pendrive) {
                    messages_.push_back(msg("snapshot: pendrive left out: %s", m.pendrive.c_str()));
                }
            }
        }
        if (!folder.empty() && (!folder_ || folder_->dir() != fs::path(folder)) && !set_folder(folder, why)) {
            messages_.push_back(msg("snapshot: folder left out: %s", why.c_str()));
        }
        if (cassette.empty()) {
            detach();
        } else if (attached_path_ != fs::path(cassette) && !attach(cassette, why)) {
            messages_.push_back(msg("snapshot: cassette left out: %s", why.c_str()));
        }
    }
    return true;
}

std::string HostTape::game_name() const {
    if (!program_name_.empty()) {
        return program_name_;
    }
    // A card's folder only while the card is in: the front end keeps it attached across the card's absence
    const fs::path nanosd_dir = nanosd_slot() >= 0 ? nanosd_sd_dir() : fs::path();
    const fs::path pendrive_dir = card256_slot() >= 0 ? card256_host_.pendrive_dir() : fs::path();
    for (const fs::path& dir : {nanosd_dir, pendrive_dir}) {
        if (!dir.empty()) {
            return dir.filename().empty() ? dir.parent_path().filename().string() : dir.filename().string();
        }
    }
    // The GameCard's own boot menu of games
    if (card_slot(CardKind::GameCard) >= 0) {
        return "GameCard";
    }
    return {};
}

void HostTape::set_rewind(bool on) {
    rewind_on_ = on;
    rewind_points_.clear();
}

void HostTape::after_slice() {
    constexpr std::uint64_t kSecond = AudioSynth::kTickRate;
    const std::uint64_t now = machine_.ticks();
    if (!media_.busy()) {
        nanosd_media_.skip_menu = false;  // the NanoSD's menu only stays away for an autostart's boot
    }
    // A changed MultiCart flash goes back to its file after half a second of machine time without writes
    if (multicart_flash_.dirty && (now < multicart_flash_.last_write || now - multicart_flash_.last_write >= kSecond / 2)) {
        save_multicart();
    }
    // A written disk goes back to its file once the drive has been idle for half a second of machine time
    for (unsigned d = 0; d < FloppyDrives::kDrives; ++d) {
        if (drives_.dirty(d) && !drives_.active(d, now, kSecond / 2)) {
            std::string error;
            if (!save_disk(d, error)) {
                messages_.push_back(error);
            }
        }
    }
    printer_.flush();
    serial_.flush();
    if (!rewind_on_) {
        return;
    }
    // The clock restarts on power-on and when another state is loaded: what was kept no longer leads here
    if (!rewind_points_.empty() && now < rewind_points_.back().ticks) {
        rewind_points_.clear();
    }
    if (!rewind_points_.empty() && now - rewind_points_.back().ticks < kSecond) {
        return;
    }
    rewind_points_.push_back({now, snapshot_bytes()});
    while (rewind_points_.size() > kRewindSeconds + 1) {
        rewind_points_.pop_front();
    }
}

bool HostTape::rewind(unsigned seconds, std::string& error) {
    const std::uint64_t target = AudioSynth::kTickRate * std::max(1u, seconds);
    const std::uint64_t now = machine_.ticks();
    while (!rewind_points_.empty() && now - std::min(now, rewind_points_.back().ticks) < target) {
        if (rewind_points_.size() == 1) {
            break;
        }
        rewind_points_.pop_back();
    }
    if (rewind_points_.empty() || rewind_points_.back().ticks >= now) {
        error = rewind_on_ ? msg("nothing kept to rewind to yet") : msg("rewind is off in this front end");
        return false;
    }
    const RewindPoint point = rewind_points_.back();
    if (!snapshot_load_bytes(point.bytes, msg("rewind"), error)) {
        rewind_points_.clear();
        return false;
    }
    return true;
}

std::string HostTape::rewind_describe() const {
    if (!rewind_on_) {
        return msg("rewind off");
    }
    if (rewind_points_.empty()) {
        return msg("rewind: nothing kept yet");
    }
    const std::uint64_t span = (machine_.ticks() - std::min(machine_.ticks(), rewind_points_.front().ticks)) / AudioSynth::kTickRate;
    return msg("rewind: %llu s back possible, %zu points, %zu KiB", static_cast<unsigned long long>(span), rewind_points_.size(),
               rewind_bytes() / 1024);
}

std::size_t HostTape::rewind_bytes() const {
    std::size_t total = 0;
    for (const RewindPoint& p : rewind_points_) {
        total += p.bytes.size();
    }
    return total;
}

std::string HostTape::describe() const {
    const HostCassette& c = media_.cassette();
    const std::string cassette = c.attached() ? msg("cassette %s (%s)", c.attached_name().c_str(), attached_path_.string().c_str()) : msg("no cassette");
    if (!folder_) {
        return cassette;
    }
    return msg(folder_->writable() ? N_("%s, folder %s (read/write)") : N_("%s, folder %s (read only)"), cassette.c_str(), folder_->dir().string().c_str());
}

std::vector<std::string> HostTape::take_messages() {
    std::vector<std::string> out;
    out.swap(messages_);
    for (std::string& m : media_.cassette().take_messages()) {
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::uint8_t> HostTape::gamecard_rom() {
    if (!gamecard_rom_known_) {
        gamecard_rom_known_ = true;
        std::string why;
        if (auto rom = find_card_rom(kGameCardRom, 0x10000, rom_places_, why)) {
            gamecard_rom_ = std::move(*rom);
        } else {
            messages_.push_back(msg("GameCard without its EPROM, so without the boot menu: %s", why.substr(0, why.find("; searched")).c_str()));
        }
    }
    return gamecard_rom_;
}

int HostTape::card_slot(CardKind kind) const {
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (machine_.config().cards[i].kind == kind) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HostTape::gamecard_slot() const { return card_slot(CardKind::GameCard); }

bool HostTape::gamecard(bool on, unsigned slot, std::string& error) { return place_card(CardKind::GameCard, &HostTape::gamecard_rom, on, slot, error); }

bool HostTape::place_card(CardKind kind, std::vector<std::uint8_t> (HostTape::*rom)(), bool on, unsigned slot, std::string& error) {
    program_name_.clear();  // another machine: the last program's name no longer fits
    MachineConfig config = machine_.config();
    const int now = card_slot(kind);
    if (!on) {
        if (now < 0) {
            return true;
        }
        config.cards[static_cast<unsigned>(now)] = {};
    } else {
        if (slot >= kCardSlots) {
            error = msg("the slots are 0 to 3");
            return false;
        }
        if (now == static_cast<int>(slot)) {
            return true;
        }
        if (config.cards[slot].kind != CardKind::None) {
            error = msg("slot %u holds the %s", slot, card_kind_name(config.cards[slot].kind));
            return false;
        }
        if (now >= 0) {
            config.cards[static_cast<unsigned>(now)] = {};
        }
        config.cards[slot] = {kind, (this->*rom)()};
    }
    media_.cancel();
    machine_.configure(config);
    return true;
}

std::vector<std::uint8_t> HostTape::multichip_rom() {
    if (!multichip_rom_known_) {
        multichip_rom_known_ = true;
        std::string why;
        if (auto rom = find_card_rom(kMultichipRom, MultichipCard::kRomSize, rom_places_, why)) {
            multichip_rom_ = std::move(*rom);
        } else {
            messages_.push_back(msg("Multichip sound card without its EEPROM, so without its start-up screen: %s", why.substr(0, why.find("; searched")).c_str()));
        }
    }
    return multichip_rom_;
}

int HostTape::multichip_slot() const { return card_slot(CardKind::Multichip); }

bool HostTape::multichip(bool on, unsigned slot, std::string& error) { return place_card(CardKind::Multichip, &HostTape::multichip_rom, on, slot, error); }

bool HostTape::twodfx(bool on, unsigned slot, std::string& error) {
    if (on && card_slot(CardKind::Card256) >= 0) {
        error = msg("not with the TVC256k++ (both drive EC0-3 and /EXTC)");
        return false;
    }
    if (on && slot == kCardSlots) {
        const auto& cards = machine_.config().cards;
        const int now = card_slot(CardKind::TwoDfx);
        slot = now >= 0 ? static_cast<unsigned>(now) : 2;
        for (unsigned s = 0; s < kCardSlots && now < 0 && cards[slot].kind != CardKind::None; ++s) {
            slot = s;
        }
        if (now < 0 && cards[slot].kind != CardKind::None) {
            error = msg("every slot holds a card");
            return false;
        }
    }
    return place_card(CardKind::TwoDfx, &HostTape::no_rom, on, slot, error);
}

int HostTape::twodfx_slot() const { return card_slot(CardKind::TwoDfx); }

std::string HostTape::twodfx_describe() const {
    const int slot = twodfx_slot();
    if (slot < 0) {
        return "no 2dfx graphics card";
    }
    std::string text = "2dfx graphics card in slot " + std::to_string(slot) + " (ports F8h-F9h)";
    if (const auto* card = dynamic_cast<const TwoDfxCard*>(machine_.cards().card(static_cast<unsigned>(slot)))) {
        char st[64];
        std::snprintf(st, sizeof st, ", status %02Xh, /INT %s", card->status(), card->irq() ? "low" : "high");
        text += st;
    }
    return text;
}

std::string HostTape::multichip_describe() const {
    const int slot = multichip_slot();
    if (slot < 0) {
        return "no Multichip sound card";
    }
    const std::vector<std::uint8_t>& rom = machine_.config().cards[static_cast<unsigned>(slot)].rom;
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", static_cast<unsigned>(crc32(rom)));
    std::string text = "Multichip sound card in slot " + std::to_string(slot) + " (ports " + std::to_string(slot + 1) + "0h-" +
                       std::to_string(slot + 1) + "Fh), " + (rom.empty() ? std::string("no EEPROM image") : std::string("EEPROM CRC-32 ") + crc);
    if (const auto* card = dynamic_cast<const MultichipCard*>(machine_.cards().card(static_cast<unsigned>(slot)))) {
        text += ", bank " + std::to_string(card->bank()) + ", SAA1099 " + (card->saa().enabled() ? "enabled" : "disabled") +
                ", YM3812 /IRQ " + (card->irq() ? "low" : "high");
    }
    return text;
}

bool HostTape::sid_card(const std::string& model, std::string& error) {
    program_name_.clear();  // another machine: the last program's name no longer fits
    MachineConfig config = machine_.config();
    if (model == "off") {
        config.sid.model = SidModel::None;
    } else if (model == "6581") {
        config.sid.model = SidModel::Mos6581;
    } else if (model == "8580") {
        config.sid.model = SidModel::Mos8580;
    } else {
        error = msg("the SID card is off, 6581 or 8580");
        return false;
    }
    if (config.sid.model != SidModel::None && machine_.config().sid.model == SidModel::None) {
        config.sid.clock_hz = sid_clock_hz_;
    }
    if (config.sid == machine_.config().sid) {
        return true;
    }
    media_.cancel();
    machine_.configure(config);
    return true;
}

std::string HostTape::sid_card_model() const {
    switch (machine_.config().sid.model) {
    case SidModel::Mos6581:
        return "6581";
    case SidModel::Mos8580:
        return "8580";
    default:
        return "off";
    }
}

std::string HostTape::sid_card_describe() const {
    const SidCard* sid = machine_.sid_card();
    if (!sid) {
        return "no SID card";
    }
    std::string text = "SID card, MOS " + sid_card_model() + " at ports E0h-FFh, clock " + std::to_string(sid->config().clock_hz) + " Hz";
    char line[96];
    std::snprintf(line, sizeof line, ", volume %u, filter %02Xh/%02Xh, gates", sid->peek(0x18) & 15u, sid->peek(0x17), sid->peek(0x18) & 0xF0u);
    text += line;
    for (unsigned v = 0; v < 3; ++v) {
        text += (sid->peek(4 + 7 * v) & 1) ? " " + std::to_string(v + 1) : std::string(" -");
    }
    return text;
}

std::string HostTape::gamecard_describe() const {
    const int slot = gamecard_slot();
    if (slot < 0) {
        return "no GameCard";
    }
    const std::vector<std::uint8_t>& rom = machine_.config().cards[static_cast<unsigned>(slot)].rom;
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", static_cast<unsigned>(crc32(rom)));
    std::string text = "GameCard in slot " + std::to_string(slot) + " (ports " + std::to_string(slot + 1) + "0h-" + std::to_string(slot + 1) +
                       "Fh), " + (rom.empty() ? std::string("no EPROM image") : std::string("EPROM CRC-32 ") + crc);
    if (const auto* card = dynamic_cast<const GameCard*>(machine_.cards().card(static_cast<unsigned>(slot)))) {
        text += ", page " + std::to_string(card->page()) + ", sound clock " + (card->sound().running() ? "on" : "off");
    }
    return text;
}

std::vector<std::uint8_t> HostTape::card256_rom() {
    if (!card256_rom_known_) {
        card256_rom_known_ = true;
        std::string why;
        if (auto rom = find_card_rom(kCard256Rom, Card256::kRomSize, rom_places_, why)) {
            card256_rom_ = std::move(*rom);
        } else {
            messages_.push_back(msg("TVC256k++ without its ROM, so the machine never calls the card: %s", why.substr(0, why.find("; searched")).c_str()));
        }
    }
    if (card256_image_.empty()) {
        card256_image_ = card256_image(card256_rom_, rom_places_, messages_);
    }
    if (!card256_flash_known_ && !card256_host_.has_flash()) {
        card256_flash_known_ = true;
        std::string why;
        if (auto image = find_card_rom(kCard256Flash, kCard256FlashBytes, rom_places_, why)) {
            card256_host_.set_flash(std::move(*image), std::string(kCard256Flash));
            if (!card256_host_.media().flash_valid) {
                messages_.push_back(msg("TVC256k++: %s does not mount as its flash drive", std::string(kCard256Flash).c_str()));
            }
        } else {
            messages_.push_back(msg("TVC256k++ without its flash drive, so a bare LOAD there finds nothing: %s", why.substr(0, why.find("; searched")).c_str()));
        }
    }
    return card256_image_;
}

bool HostTape::refresh_card_roms() {
    if (!gamecard_rom_given_) {
        gamecard_rom_known_ = false;
        gamecard_rom_.clear();
    }
    if (!multichip_rom_given_) {
        multichip_rom_known_ = false;
        multichip_rom_.clear();
    }
    if (!card256_rom_given_) {
        card256_rom_known_ = false;
        card256_rom_.clear();
    }
    if (!nanosd_rom_given_) {
        nanosd_rom_known_ = false;
        nanosd_rom_.clear();
    }
    card256_image_.clear();
    if (!card256_host_.has_flash()) {
        card256_flash_known_ = false;
    }
    MachineConfig config = machine_.config();
    bool changed = false;
    for (CardConfig& card : config.cards) {
        std::vector<std::uint8_t> image;
        if (card.kind == CardKind::GameCard) {
            image = gamecard_rom();
        } else if (card.kind == CardKind::Multichip) {
            image = multichip_rom();
        } else if (card.kind == CardKind::NanoSd) {
            image = nanosd_rom();
        } else if (card.kind == CardKind::Card256) {
            image = card256_rom();
        } else {
            continue;
        }
        if (image != card.rom) {
            card.rom = std::move(image);
            changed = true;
        }
    }
    if (changed) {
        media_.cancel();
        machine_.configure(config);
    }
    return changed;
}

bool HostTape::card256_usb(const std::string& what, std::string& error) {
    if (what == "none") {
        card256_host_.unplug();
    } else if (what == "mouse") {
        card256_host_.plug_mouse();
    } else if (!card256_host_.plug_pendrive(what, error)) {
        return false;
    }
    return true;
}

bool HostTape::card256_flash(const std::string& path, std::string& error) {
    if (!card256_host_.load_flash(path, error)) {
        return false;
    }
    card256_flash_known_ = true;
    return true;
}

bool HostTape::card256_mouse(int dx, int dy, unsigned buttons, std::string& error) {
    const int slot = card256_slot();
    auto* card = slot < 0 ? nullptr : dynamic_cast<Card256*>(machine_.card(static_cast<unsigned>(slot)));
    if (!card) {
        error = msg("no TVC256k++");
        return false;
    }
    if (card256_host_.usb() != Card256Usb::Mouse) {
        error = msg("no mouse on the TVC256k++'s USB port");
        return false;
    }
    card->set_usb(Card256Usb::Mouse);
    // A boot-protocol report carries at most 127 each way: a larger move goes out as several
    do {
        const int x = std::clamp(dx, -127, 127);
        const int y = std::clamp(dy, -127, 127);
        card->mouse_report(x, y, static_cast<std::uint8_t>(buttons & 7));
        dx -= x;
        dy -= y;
    } while (dx != 0 || dy != 0);
    return true;
}

std::vector<std::uint8_t> HostTape::nanosd_rom() {
    if (!nanosd_rom_known_) {
        nanosd_rom_known_ = true;
        std::string why;
        if (auto rom = find_card_rom(kNanoSdRom, NanoSd::kRomSize, rom_places_, why)) {
            nanosd_rom_ = std::move(*rom);
        } else {
            messages_.push_back(msg("NanoSD without its EEPROM, so the machine never calls the card: %s", why.substr(0, why.find("; searched")).c_str()));
        }
    }
    return nanosd_rom_;
}

int HostTape::nanosd_slot() const { return card_slot(CardKind::NanoSd); }

bool HostTape::nanosd(bool on, unsigned slot, std::string& error) { return place_card(CardKind::NanoSd, &HostTape::nanosd_rom, on, slot, error); }

bool HostTape::nanosd_sd(const std::string& dir, std::string& error) {
    nanosd_media_.sd_changed = machine_.ticks();
    if (dir == "none") {
        nanosd_media_.sd = nullptr;
        nanosd_folder_.reset();
        return true;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        error = msg("'%s' is not a directory", dir.c_str());
        return false;
    }
    nanosd_media_.sd = nullptr;
    nanosd_folder_.emplace(fs::absolute(dir, ec).lexically_normal());
    nanosd_media_.sd = &*nanosd_folder_;
    return true;
}

std::string HostTape::nanosd_describe() const {
    const int slot = nanosd_slot();
    const std::string sd = nanosd_folder_ ? "SD card " + nanosd_folder_->root().string() : std::string("no SD card");
    if (slot < 0) {
        return "no NanoSD; " + sd;
    }
    const std::vector<std::uint8_t>& rom = machine_.config().cards[static_cast<unsigned>(slot)].rom;
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", static_cast<unsigned>(crc32(rom)));
    std::string text = "NanoSD in slot " + std::to_string(slot) + " (ports " + std::to_string(slot + 1) + "0h-" + std::to_string(slot + 1) +
                       "Fh), " + (rom.empty() ? std::string("no EEPROM image") : std::string("EEPROM CRC-32 ") + crc);
    if (const auto* card = dynamic_cast<const NanoSd*>(machine_.cards().card(static_cast<unsigned>(slot))); card && !card->booted()) {
        text += ", bank 0; Nano: starts at the machine's first access to the card";
    } else if (card) {
        char nano[160];
        std::snprintf(nano, sizeof nano, ", bank %u; Nano: status %02X, latch %02X, %zu in / %zu out queued, SD %s%s", card->bank(),
                      card->status(), card->latch(), card->input_size(), card->output_size(), card->sd_ready() ? "ready" : "not ready",
                      card->hung() ? ", hung (power on to restart it)" : "");
        text += nano;
        text += ", directory " + card->current_dir();
        if (!card->open_file().empty()) {
            text += ", file " + card->open_file();
        }
    }
    text += "; " + sd + "; EEPROM: ";
    text += nanosd_media_.eeprom_valid ? "menu at boot " + std::string(nanosd_media_.menu ? "on" : "off") + ", sort " + std::to_string(nanosd_media_.sort)
                                       : std::string("not written yet (menu at boot)");
    return text;
}

int HostTape::card256_slot() const {
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (machine_.config().cards[i].kind == CardKind::Card256) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool HostTape::card256(bool on, unsigned slot, std::string& error) {
    program_name_.clear();  // another machine: the last program's name no longer fits
    MachineConfig config = machine_.config();
    const int now = card256_slot();
    if (!on) {
        if (now < 0) {
            return true;
        }
        config.cards[static_cast<unsigned>(now)] = {};
    } else {
        if (card_slot(CardKind::TwoDfx) >= 0) {
            error = msg("the TVC256k++ cannot go in beside the 2dfx card (both drive EC0-3 and /EXTC)");
            return false;
        }
        if (config.model == Model::Tvc32kHbm) {
            error = msg("the TVC256k++ cannot share the machine with the HBM RAM card (its manual: no other RAM expansion beside it)");
            return false;
        }
        if (now >= 0 && (slot == kCardSlots || now == static_cast<int>(slot))) {
            return true;
        }
        if (now >= 0) {
            config.cards[static_cast<unsigned>(now)] = {};
        }
        if (slot == kCardSlots) {
            slot = card256_default_slot(config.cards);
            if (slot == kCardSlots) {
                error = msg("every slot holds a card");
                return false;
            }
        } else if (slot > kCardSlots) {
            error = msg("the slots are 0 to 3");
            return false;
        } else if (config.cards[slot].kind != CardKind::None) {
            error = msg("slot %u holds the %s", slot, card_kind_name(config.cards[slot].kind));
            return false;
        }
        config.cards[slot] = {CardKind::Card256, card256_rom()};
    }
    media_.cancel();
    machine_.configure(config);
    return true;
}

std::string HostTape::card256_describe() const {
    const int slot = card256_slot();
    if (slot < 0) {
        return "no TVC256k++";
    }
    const std::vector<std::uint8_t>& image = machine_.config().cards[static_cast<unsigned>(slot)].rom;
    const std::span<const std::uint8_t> rom(image.data(), std::min(image.size(), Card256::kRomSize));
    const bool no_rom = std::all_of(rom.begin(), rom.end(), [](std::uint8_t b) { return b == 0xFF; });
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", static_cast<unsigned>(crc32(rom)));
    std::string text = "TVC256k++ in slot " + std::to_string(slot) + " (ports " + std::to_string(slot + 1) + "0h-" + std::to_string(slot + 1) +
                       "Fh, EXT " + std::to_string(slot) + "), " + (no_rom ? std::string("no ROM image") : std::string("ROM CRC-32 ") + crc) +
                       (image.size() > Card256::kRomSize ? ", font and picture" : ", no font or picture");
    if (const auto* card = dynamic_cast<const Card256*>(machine_.cards().card(static_cast<unsigned>(slot)))) {
        const auto page = [](std::uint8_t p) { return p == 0xFF ? std::string("own RAM") : std::to_string(p); };
        text += ", U2 page " + page(card->unit_page(0)) + ", U3 page " + page(card->unit_page(1)) + ", ROM bank " + std::to_string(card->rom_bank()) +
                ", PSRAM pages in use " + std::to_string(card->psram_pages_used());
        static constexpr const char* kModes[] = {"sprites only", "2-colour text", "16-colour text", "bitmap"};
        const auto word = [&](std::uint8_t lo) { return card->peek_register(lo) | card->peek_register(static_cast<std::uint8_t>(lo + 1)) << 8; };
        char overlay[160];
        std::snprintf(overlay, sizeof overlay, "; overlay: %s, line counter %u, sprites on %04X, collisions %04X/%04X, /INT %s",
                      kModes[card->peek_register(0xA0) & 3], card->peek_register(0xA8), static_cast<unsigned>(word(0x90)),
                      static_cast<unsigned>(word(0x96)), static_cast<unsigned>(word(0x98)), card->irq() ? "low" : "high");
        text += overlay;
        static constexpr const char* kDrives[] = {"pendrive", "PSRAM", "flash", "DSK"};
        const std::uint8_t drive = card->peek_register(0xC5);
        text += "; drive " + std::string(drive < 4 ? kDrives[drive] : "?") + ", PSRAM drive " + (card->psram_drive() ? "on" : "off") +
                ", DSK " + (card->dsk_kind() ? card->dsk_path() : std::string("none"));
        // The vSID as its registers read back
        char vsid[96];
        std::snprintf(vsid, sizeof vsid, "; vSID: volume %u, gates %u%u%u, ENV3 %02X",
                      card->peek_register(0xF8) & 15u, card->peek_register(0xE4) & 1u, card->peek_register(0xEB) & 1u,
                      card->peek_register(0xF2) & 1u, card->peek_register(0xFC));
        text += vsid;
    }
    text += "; " + card256_host_.describe();
    return text;
}

bool HostTape::is_disk_file(const std::string& path) {
    const std::string ext = fs::path(path).extension().string();
    return same_ascii(ext, ".dsk") || same_ascii(ext, ".img");
}

std::optional<unsigned> HostTape::parse_drive(std::string_view text) {
    if (text.size() == 2 && text[1] == ':') {
        text.remove_suffix(1);
    }
    if (text.size() != 1) {
        return std::nullopt;
    }
    const char c = static_cast<char>(text[0] | 0x20);
    if (c < 'a' || c >= static_cast<char>('a' + FloppyDrives::kDrives)) {
        return std::nullopt;
    }
    return static_cast<unsigned>(c - 'a');
}

bool HostTape::disk_card_on() const { return machine_.config().cards[kDiskCardSlot].kind == CardKind::Hbf; }

bool HostTape::disk_card(bool on, std::string& error) {
    if (on == disk_card_on()) {
        return true;
    }
    program_name_.clear();  // another machine: the last program's name no longer fits
    MachineConfig config = machine_.config();
    if (on && config.cards[kDiskCardSlot].kind != CardKind::None) {
        error = msg("slot %u, the disk card's, holds the %s", static_cast<unsigned>(kDiskCardSlot), card_kind_name(config.cards[kDiskCardSlot].kind));
        return false;
    }
    if (on) {
        if (disk_rom_.empty()) {
            auto rom = find_disk_card_rom(rom_places_, error);
            if (!rom) {
                return false;
            }
            disk_rom_ = std::move(*rom);
        }
        config.cards[kDiskCardSlot] = {CardKind::Hbf, disk_rom_};
    } else {
        config.cards[kDiskCardSlot] = {};
    }
    media_.cancel();
    machine_.configure(config);
    return true;
}

bool HostTape::insert_disk_file(unsigned drive, const std::string& path, std::string& error) {
    if (drive >= FloppyDrives::kDrives) {
        error = msg("there are drives A: to D: only");
        return false;
    }
    constexpr std::size_t kMaxDisk = std::size_t{2} * DiskImage::kMaxTracks * 11 * FloppyGeometry::kSectorSize;
    auto bytes = read_file(path, kMaxDisk, error);
    if (!bytes) {
        return false;
    }
    auto image = DiskImage::from_bytes(std::move(*bytes), error);
    if (!image) {
        error = msg("%s: %s", fs::path(path).filename().string().c_str(), error.c_str());
        return false;
    }
    // A file the host will not let us write is a write-protected disk
    const bool writable = static_cast<bool>(std::fstream(path, std::ios::in | std::ios::out | std::ios::binary));
    if (drives_.disk(drive) && !disk_eject(drive, error)) {
        return false;
    }
    drives_.insert(drive, std::move(*image), !writable);
    disk_paths_[drive] = path;
    return true;
}

bool HostTape::disk_insert(unsigned drive, const std::string& path, std::string& error) {
    if (!disk_card_on() && !disk_card(true, error)) {
        return false;
    }
    if (!insert_disk_file(drive, path, error)) {
        return false;
    }
    program_name_ = fs::path(path).stem().string();
    return true;
}

bool HostTape::save_disk(unsigned drive, std::string& error) {
    const DiskImage* disk = drives_.disk(drive);
    if (!disk || !drives_.dirty(drive)) {
        return true;
    }
    // Cleared first: a disk that cannot be written is reported once, not after every slice
    drives_.clear_dirty(drive);
    const fs::path& target = disk_paths_[drive];
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(disk->bytes().data()), static_cast<std::streamsize>(disk->bytes().size()));
        if (!out) {
            error = msg("cannot write the disk in %c: to %s", static_cast<char>('A' + drive), temp.string().c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        error = msg("cannot write the disk in %c: to %s: %s", static_cast<char>('A' + drive), target.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool HostTape::save_disks() {
    bool ok = true;
    for (unsigned d = 0; d < FloppyDrives::kDrives; ++d) {
        std::string error;
        if (!save_disk(d, error)) {
            messages_.push_back(error);
            ok = false;
        }
    }
    return ok;
}

bool HostTape::disk_eject(unsigned drive, std::string& error) {
    if (drive >= FloppyDrives::kDrives) {
        error = msg("there are drives A: to D: only");
        return false;
    }
    if (!save_disk(drive, error)) {
        return false;
    }
    drives_.eject(drive);
    disk_paths_[drive].clear();
    return true;
}

bool HostTape::disk_autostart(const std::string& path, std::string& error) {
    // The first empty drive while the card is in (A: when all are full); the program starts from there
    unsigned drive = 0;
    if (disk_card_on()) {
        for (unsigned d = 0; d < FloppyDrives::kDrives; ++d) {
            if (!drives_.disk(d)) {
                drive = d;
                break;
            }
        }
    }
    if (!disk_insert(drive, path, error)) {
        return false;
    }
    const std::vector<std::string> files = drives_.disk(drive)->root_files();
    const auto cas = std::find_if(files.begin(), files.end(), [](const std::string& f) {
        return f.size() > 4 && same_ascii(std::string_view(f).substr(f.size() - 4), ".CAS");
    });
    if (cas == files.end()) {
        error = msg("%s: no CAS file in the root directory to start", fs::path(path).filename().string().c_str());
        return false;
    }
    program_name_ = fs::path(path).stem().string();
    keep_prompt_state();
    fetch_prompt_state();
    nanosd_media_.skip_menu = nanosd_slot() >= 0;
    const std::string prefix = drive == 0 ? std::string() : std::string(1, static_cast<char>('A' + drive)) + ":";
    media_.autostart_disk(machine_, prefix + cas->substr(0, cas->size() - 4));
    return true;
}

std::string HostTape::disk_name(unsigned drive) const {
    return drive < FloppyDrives::kDrives && drives_.disk(drive) ? disk_paths_[drive].filename().string() : std::string();
}

std::string HostTape::disk_describe() const {
    std::string text;
    if (disk_card_on()) {
        char crc[16];
        std::snprintf(crc, sizeof crc, "%08X", static_cast<unsigned>(crc32(machine_.config().cards[kDiskCardSlot].rom)));
        text = "disk card in slot " + std::to_string(kDiskCardSlot) + " (ROM CRC-32 " + crc + ")\n";
    } else {
        text = "disk card out (drives keep their disks)\n";
    }
    for (unsigned d = 0; d < FloppyDrives::kDrives; ++d) {
        text += std::string(1, static_cast<char>('A' + d)) + ": ";
        const DiskImage* disk = drives_.disk(d);
        if (!disk) {
            text += "empty\n";
            continue;
        }
        const FloppyGeometry& g = disk->geometry();
        text += disk_paths_[d].filename().string() + " (" + std::to_string(g.bytes() / 1024) + " KiB, " + std::to_string(g.tracks) +
                " tracks x " + std::to_string(g.heads) + (g.heads == 1 ? " side x " : " sides x ") + std::to_string(g.sectors) + " sectors)";
        if (drives_.write_protected(d)) {
            text += ", write-protected";
        }
        if (drives_.dirty(d)) {
            text += ", written (not saved yet)";
        }
        text += "\n";
    }
    text.pop_back();
    return text;
}


bool HostTape::sddual(bool on, bool gamer, std::string& error) {
    program_name_.clear();  // another machine: the last program's name no longer fits
    MachineConfig config = machine_.config();
    save_multicart();
    if (!on) {
        if (config.roms.cart_type == CartType::SdDual) {
            cart_remove();
        }
        return true;
    }
    std::vector<std::uint8_t> rom = sddual_rom_;
    const std::string_view file = gamer ? kSdDualGamerRom : kSdDualRom;
    if (rom.empty() || gamer) {
        auto found = find_card_rom(file, SdDual::kFlashSize, rom_places_, error);
        if (!found) {
            return false;
        }
        rom = std::move(*found);
    }
    if (!is_sddual_firmware(rom)) {
        error = msg("%s is not SD Dual firmware", std::string(file).c_str());
        return false;
    }
    drop_multicart();
    config.roms.cart = std::move(rom);
    config.roms.cart_type = CartType::SdDual;
    config.roms.cart_bank = sddual_writable_ ? 1 : 0;
    media_.cancel();
    machine_.configure(config);
    cart_path_ = std::string(sddual_rom_.empty() || gamer ? file : std::string_view("--sddual-rom"));
    return true;
}

bool HostTape::sd_insert(unsigned socket, const std::string& path, std::string& error) {
    socket &= 1;
    machine_.set_sd_image(socket, nullptr);
    sd_files_[socket].close();
    if (!sd_files_[socket].open(path, error)) {
        return false;
    }
    machine_.set_sd_image(socket, &sd_files_[socket]);
    return true;
}

void HostTape::sd_eject(unsigned socket) {
    socket &= 1;
    machine_.set_sd_image(socket, nullptr);
    sd_files_[socket].close();
}

std::string HostTape::sd_describe() const {
    std::string out = sddual_on() ? cart_describe() : std::string("SD Dual: out");
    for (unsigned s = 0; s < sd_files_.size(); ++s) {
        const SdImageFile& f = sd_files_[s];
        out += std::string("\n") + (s == 0 ? "microSD: " : "SD: ");
        if (!f.is_open()) {
            out += "empty";
            continue;
        }
        out += f.path().filename().string() + ", " + std::to_string(f.size() >> 20) + " MiB, " +
               (SdCard::high_capacity(f) ? "SDHC" : "SDSC") + (f.read_only() ? ", read-only" : "") +
               (s == 1 && f.read_only() ? " (write-protect switch on)" : "") + (f.failed() ? ", a read or write failed" : "");
    }
    return out;
}

bool HostTape::sd_build(const std::string& out, std::uint64_t mib, const std::vector<std::string>& files, std::string& error) {
    for (const SdImageFile& f : sd_files_) {
        std::error_code ec;
        if (f.is_open() && fs::equivalent(f.path(), out, ec)) {
            error = msg("%s is in an SD socket", out.c_str());
            return false;
        }
    }
    return write_sd_image(out, mib, files, error);
}

bool HostTape::printer_plug(const std::string& raw, const std::string& text, std::string& error) {
    if (!printer_.plug(raw, text, error)) {
        return false;
    }
    printer_raw_ = raw;
    printer_text_ = text;
    machine_.set_printer(&printer_);
    return true;
}

void HostTape::printer_unplug() {
    printer_.unplug();
    printer_raw_.clear();
    printer_text_.clear();
    machine_.set_printer(nullptr);
}

std::string HostTape::printer_setting() const {
    if (!printer_.plugged()) {
        return "off";
    }
    if (printer_raw_.empty() && printer_text_.empty()) {
        return "on";
    }
    return printer_raw_.empty() ? printer_text_ : printer_raw_;
}

int HostTape::serial_slot() const { return card_slot(CardKind::Serial); }

bool HostTape::serial_card(bool on, unsigned slot, std::string& error) {
    return place_card(CardKind::Serial, &HostTape::no_rom, on, slot, error);
}

std::string HostTape::serial_describe() const {
    const int slot = serial_slot();
    std::string text = slot < 0 ? std::string("no serial card")
                                : "serial card in slot " + std::to_string(slot) + " (ports " + std::to_string(slot + 1) + "0h-" +
                                      std::to_string(slot + 1) + "Fh)";
    if (const auto* card = slot < 0 ? nullptr : dynamic_cast<const SerialCard*>(machine_.cards().card(static_cast<unsigned>(slot)))) {
        const I8251& usart = card->usart();
        char state[160];
        if (usart.expecting_mode()) {
            std::snprintf(state, sizeof state, ", 8251A waiting for its mode, status %02X", card->status(machine_.card_view()));
        } else {
            // TXRXCLK is 3.125 MHz / (4096 - P) / 2, divided by the mode's factor
            const std::uint16_t pitch = machine_.sound().pitch();
            const unsigned factor = (usart.mode() & 3) == 3 ? 64 : (usart.mode() & 3) == 2 ? 16 : 1;
            const double baud = (usart.mode() & 3) == 0 || pitch == 0xFFF ? 0.0 : 1'562'500.0 / (0x1000 - pitch) / factor;
            std::snprintf(state, sizeof state, ", 8251A mode %02X command %02X status %02X, %.0f baud from pitch %03X", usart.mode(), usart.command(),
                          card->status(machine_.card_view()), baud, pitch);
        }
        text += state;
    }
    return text + "; " + serial_.describe();
}

}  // namespace freetvc
