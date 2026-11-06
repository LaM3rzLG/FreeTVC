// SPDX-License-Identifier: GPL-3.0-or-later
// Host files around the machine (design/media.md, design/cards.md): cassette, tape, cartridge, snapshots and disks.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/cards/floppy.hpp"
#include "core/cards/nanosd.hpp"
#include "core/media/media_control.hpp"
#include "frontend/common/printer_host.hpp"
#include "frontend/common/serial_host.hpp"
#include "frontend/common/card256_host.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/common/sd_image.hpp"

namespace freetvc {

// Named files in one directory: LOAD "NAME" opens NAME.CAS (ASCII case ignored), SAVE "NAME" writes NAME.CAS.
class CasFolder final : public CassetteFolder {
public:
    CasFolder(std::filesystem::path dir, bool writable) : dir_(std::move(dir)), writable_(writable) {}
    std::optional<std::vector<std::uint8_t>> read(std::string_view name, std::string& found) override;
    bool write(std::string_view name, std::span<const std::uint8_t> cas, std::string& error) override;
    bool writable() const override { return writable_; }
    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path dir_;
    bool writable_;
};

// "plain", "kilocart", "megacart", "multicart", "sddual"; nullopt for anything else.
std::optional<CartType> parse_cart_type(std::string_view name);
const char* cart_type_name(CartType type);
// A cartridge image into `roms`: type "" chooses a MultiCart v2 or the SD Dual by its firmware's
// header, else by size (up to 16 KiB plain, up to 64 KiB KiloCart, up to 1 MiB MegaCart); false with `error` set. A MultiCart's flash goes into
// `flash` (two chips, FFh past the file), never into `roms`; without `flash` a MultiCart image is an error.
bool load_cart(const std::string& path, const std::string& type, unsigned bank, RomImages& roms, std::string& error,
               std::vector<std::uint8_t>* flash = nullptr);

class HostTape final : public MediaControl {
public:
    // The machine must outlive the tape; its OS-call handler becomes media().
    explicit HostTape(Machine& machine);
    ~HostTape() override;
    HostTape(const HostTape&) = delete;
    HostTape& operator=(const HostTape&) = delete;

    // --cas-dir: named files are read from and written to `dir`. Without it, the attached file's directory is read.
    bool set_folder(const std::string& dir, std::string& error);

    bool attach(const std::string& path, std::string& error) override;
    void detach() override;
    bool autostart(const std::string& path, std::string& error) override;
    std::string describe() const override;
    Media& media() override { return media_; }

    std::vector<std::string> take_messages() override;

    bool tape_insert(const std::string& path, std::string& error) override;
    bool tape_autostart(const std::string& path, std::string& error) override;
    bool tape_record(const std::string& path, std::string& error) override;
    void tape_play() override;
    bool tape_stop(std::string& error) override;
    void tape_rewind() override;
    bool tape_eject(std::string& error) override;
    std::string tape_describe() const override;
    // A .wav or .ttp file goes to the deck.
    static bool is_tape_file(const std::string& path);

    bool cart_insert(const std::string& path, const std::string& type, unsigned bank, std::string& error) override;
    void cart_remove() override;
    std::string cart_describe() const override;
    // A .crt, .rom or .bin file is a cartridge image.
    static bool is_cart_file(const std::string& path);
    // A MultiCart image that load_cart() put into `flash` for a machine configured with it; nothing powers on.
    void set_multicart(const std::string& path, std::vector<std::uint8_t> flash);
    // Writes a MultiCart flash the machine changed back to its file; false (messages queued) if it could not.
    bool save_multicart();
    // A new MultiCart image at `out` from the firmware in `language` ("en", "hu", "es") and the files in order; the
    // firmware comes from the ROM search (multicart-<language>.rom). Nothing goes in.
    bool multicart_build(const std::string& out, const std::vector<std::string>& files, const std::string& language,
                         std::string& error) override;
    bool sddual(bool on, bool gamer, std::string& error) override;
    bool sddual_on() const override { return machine_.config().roms.cart_type == CartType::SdDual; }
    bool sd_insert(unsigned socket, const std::string& path, std::string& error) override;
    void sd_eject(unsigned socket) override;
    std::string sd_describe() const override;
    bool sd_build(const std::string& out, std::uint64_t mib, const std::vector<std::string>& files, std::string& error) override;
    // --sddual-rom: this image instead of the ROM search's sddual.rom
    void set_sddual_rom(std::vector<std::uint8_t> rom) { sddual_rom_ = std::move(rom); }
    // The flash write jumper (--sddual-flash-write), for the cartridge's next insertion; off as the board ships
    void set_sddual_flash_writable(bool on) { sddual_writable_ = on; }
    // File name of the image in `socket`; empty when there is none
    std::string sd_name(unsigned socket) const { return sd_files_[socket & 1].path().filename().string(); }
    bool snapshot_save(const std::string& path, std::string& error) override;
    bool snapshot_load(const std::string& path, std::string& error) override;
    // The snapshot as bytes, with the MEDI chunk; and back. `from` names the source in messages.
    std::vector<std::uint8_t> snapshot_bytes();
    bool snapshot_load_bytes(std::span<const std::uint8_t> bytes, const std::string& from, std::string& error);
    // Where snapshot_load() looks for a ROM set the machine does not have.
    void set_rom_places(std::vector<RomPlace> places) { rom_places_ = std::move(places); }
    // Autostart's prompt state is kept here as one snapshot per board, ROM set and cartridge; empty = in memory only.
    void set_prompt_dir(std::filesystem::path dir) { prompt_dir_ = std::move(dir); }

    // Rewind: off until enabled; after_slice() after every run slice takes a point each second of machine time.
    static constexpr unsigned kRewindSeconds = 60;
    void set_rewind(bool on);
    void after_slice() override;
    bool rewind(unsigned seconds, std::string& error) override;
    std::string rewind_describe() const override;
    std::size_t rewind_bytes() const;

    // File name of the inserted cartridge; empty when none went in through cart_insert().
    std::string cart_name() const { return machine_.config().roms.has_cart() ? cart_path_.filename().string() : std::string(); }

    bool disk_card(bool on, std::string& error) override;
    bool disk_card_on() const override;
    bool disk_insert(unsigned drive, const std::string& path, std::string& error) override;
    bool disk_eject(unsigned drive, std::string& error) override;
    bool disk_autostart(const std::string& path, std::string& error) override;
    std::string disk_describe() const override;
    // --disk-rom: this card EPROM instead of the ROM search's hbf.rom.
    void set_disk_rom(std::vector<std::uint8_t> rom) { disk_rom_ = std::move(rom); }
    // A .dsk or .img file is a disk image.
    static bool is_disk_file(const std::string& path);
    // "A:" ... "D:" or a bare letter; nullopt for anything else.
    static std::optional<unsigned> parse_drive(std::string_view text);
    const FloppyDrives& drives() const { return drives_; }
    // File name of the disk in `drive`; empty when there is none.
    std::string disk_name(unsigned drive) const;
    // Writes every disk the machine changed back to its file; false (messages queued) if one could not be written.
    bool save_disks();

    bool gamecard(bool on, unsigned slot, std::string& error) override;
    int gamecard_slot() const override;
    std::string gamecard_describe() const override;
    // --gamecard-rom: this image instead of the ROM search's gamecard.rom.
    void set_gamecard_rom(std::vector<std::uint8_t> rom) {
        gamecard_rom_ = std::move(rom);
        gamecard_rom_known_ = gamecard_rom_given_ = true;
    }
    // The image for a GameCard: the one given, else gamecard.rom from the ROM search, else empty (a message says so).
    std::vector<std::uint8_t> gamecard_rom();
    bool multichip(bool on, unsigned slot, std::string& error) override;
    // The 2dfx graphics card (no ROM) into `slot` (kCardSlots: slot 2, else the first free) or out; powers on.
    // Not with the TVC256k++ (both drive EC0-3 and /EXTC); beside it the SID card keeps E0h-F7h (Machine::io_write).
    bool twodfx(bool on, unsigned slot, std::string& error) override;
    int twodfx_slot() const override;
    std::string twodfx_describe() const override;
    bool sid_card(const std::string& model, std::string& error) override;
    // --sid-clock: the clock a SID card gets when it goes in (default a PAL C64's)
    void set_sid_clock(std::uint32_t hz) { sid_clock_hz_ = hz; }
    std::string sid_card_model() const override;
    std::string sid_card_describe() const override;
    int multichip_slot() const override;
    std::string multichip_describe() const override;
    // --multichip-rom: this image instead of the ROM search's multichip.rom.
    void set_multichip_rom(std::vector<std::uint8_t> rom) {
        multichip_rom_ = std::move(rom);
        multichip_rom_known_ = multichip_rom_given_ = true;
    }
    // The image for a Multichip card, found like gamecard_rom().
    std::vector<std::uint8_t> multichip_rom();
    bool card256(bool on, unsigned slot, std::string& error) override;
    int card256_slot() const override;
    std::string card256_describe() const override;
    // --card256-rom: this image instead of the ROM search's card256.rom.
    void set_card256_rom(std::vector<std::uint8_t> rom) {
        card256_rom_ = std::move(rom);
        card256_rom_known_ = card256_rom_given_ = true;
        card256_image_.clear();
    }
    // After a ROM download: the card files the ROM search did not find are looked for again, and a card whose image changes
    // goes in again with it (the machine powers on); true when that happened.
    bool refresh_card_roms();
    // The TVC256k++'s image: the ROM given, else card256.rom from the ROM search, with its font and picture (card256_image);
    // messages say what is missing. Also finds the flash drive's image when none was given.
    std::vector<std::uint8_t> card256_rom();
    bool card256_usb(const std::string& what, std::string& error) override;
    bool card256_flash(const std::string& path, std::string& error) override;
    bool card256_mouse(int dx, int dy, unsigned buttons, std::string& error) override;
    Card256Host& card256_host() { return card256_host_; }
    bool nanosd(bool on, unsigned slot, std::string& error) override;
    int nanosd_slot() const override;
    std::string nanosd_describe() const override;
    bool nanosd_sd(const std::string& dir, std::string& error) override;
    // --nanosd-rom: this image instead of the ROM search's nanosd.rom.
    void set_nanosd_rom(std::vector<std::uint8_t> rom) {
        nanosd_rom_ = std::move(rom);
        nanosd_rom_known_ = nanosd_rom_given_ = true;
    }
    // The image for a NanoSD, found like gamecard_rom().
    std::vector<std::uint8_t> nanosd_rom();
    // The SD card's folder; empty when the socket is empty
    std::filesystem::path nanosd_sd_dir() const { return nanosd_folder_ ? nanosd_folder_->root() : std::filesystem::path(); }
    // The Nano's EEPROM (menu at boot, sort), kept by the front end across runs
    NanoSdMedia& nanosd_media() { return nanosd_media_; }

    bool printer_plug(const std::string& raw, const std::string& text, std::string& error) override;
    void printer_unplug() override;
    bool printer_plugged() const override { return printer_.plugged(); }
    std::uint64_t printer_printed() const { return printer_.printed(); }
    std::string printer_take() override { return printer_.take(); }
    std::string printer_describe() const override { return printer_.describe(); }
    // The printer setting's form: "off", "on", or its files joined by '|'
    std::string printer_setting() const;
    bool serial_card(bool on, unsigned slot, std::string& error) override;
    int serial_slot() const override;
    bool serial_peer(const std::string& spec, std::string& error) override { return serial_.set_peer(spec, error); }
    std::string serial_peer_name() const override { return serial_.peer(); }
    void serial_flow(bool rts) override { serial_.set_rts_flow(rts); }
    bool serial_rts_flow() const override { return serial_.rts_flow(); }
    void serial_queue(std::string_view bytes) override { serial_.queue(bytes); }
    bool serial_queue_file(const std::string& path, std::string& error) override { return serial_.queue_file(path, error); }
    bool serial_capture(const std::string& path, std::string& error) override { return serial_.set_capture(path, error); }
    std::string serial_take() override { return serial_.take(); }
    std::string serial_describe() const override;

private:
    bool save_disk(unsigned drive, std::string& error);
    bool save_multicart(std::string& error);
    // A dirty flash goes back to its file, then the flash is gone
    void drop_multicart();
    // disk_insert() without the card: the machine is not touched
    bool insert_disk_file(unsigned drive, const std::string& path, std::string& error);
    // Writes the recording to its WAV file, once per recording.
    bool save_recording(std::string& error);
    // The prompt state's file for a configuration
    std::filesystem::path prompt_file(const MachineConfig& config) const;
    // Before an autostart: the state from disk when memory has none. After one: a new state goes to disk.
    void fetch_prompt_state();
    void keep_prompt_state();
    // The slot of the first card of `kind`, or -1
    int card_slot(CardKind kind) const;
    // A card that needs no more than its image into `slot` or out (the GameCard, the Multichip card); powers on
    bool place_card(CardKind kind, std::vector<std::uint8_t> (HostTape::*rom)(), bool on, unsigned slot, std::string& error);
    // For a card without a ROM
    std::vector<std::uint8_t> no_rom() { return {}; }

    Machine& machine_;
    Media media_;
    std::filesystem::path tape_path_;    // the file in the deck
    std::filesystem::path record_path_;  // where the recording goes; empty when not recording
    std::optional<CasFolder> folder_;
    bool folder_given_ = false;
    std::filesystem::path attached_path_;
    std::filesystem::path cart_path_;
    MultiCartFlash multicart_flash_;
    std::size_t multicart_file_size_ = 0;  // a write-back keeps the file this long while the rest is still erased
    std::vector<RomPlace> rom_places_;
    std::filesystem::path prompt_dir_;
    struct RewindPoint {
        std::uint64_t ticks = 0;
        std::vector<std::uint8_t> bytes;
    };
    bool rewind_on_ = false;
    std::deque<RewindPoint> rewind_points_;
    FloppyDrives drives_;
    std::array<std::filesystem::path, FloppyDrives::kDrives> disk_paths_;
    std::vector<std::uint8_t> disk_rom_;
    std::vector<std::uint8_t> gamecard_rom_;
    bool gamecard_rom_known_ = false;  // looked for (or given) already
    bool gamecard_rom_given_ = false;  // --gamecard-rom: never searched for
    std::vector<std::uint8_t> multichip_rom_;
    std::uint32_t sid_clock_hz_ = SidCardConfig{}.clock_hz;
    bool multichip_rom_known_ = false;
    bool multichip_rom_given_ = false;
    std::vector<std::uint8_t> card256_rom_;
    bool card256_rom_known_ = false;
    bool card256_rom_given_ = false;
    std::vector<std::uint8_t> card256_image_;  // empty until built
    Card256Host card256_host_;
    bool card256_flash_known_ = false;  // looked for (or given) already
    std::vector<std::uint8_t> nanosd_rom_;
    bool nanosd_rom_known_ = false;
    bool nanosd_rom_given_ = false;
    std::optional<PendriveFolder> nanosd_folder_;
    NanoSdMedia nanosd_media_;
    PrinterHost printer_;
    std::string printer_raw_, printer_text_;
    SerialHost serial_;
    std::vector<std::uint8_t> sddual_rom_;
    bool sddual_writable_ = false;
    std::array<SdImageFile, 2> sd_files_;
    std::vector<std::string> messages_;
};

}  // namespace freetvc
