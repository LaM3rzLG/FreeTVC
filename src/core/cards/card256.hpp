// SPDX-License-Identifier: GPL-3.0-or-later
// dikdom's TVC256k++ (design/cards.md "TVC256k++"): fast RAM in place of U2/U3, PSRAM, ROM banks, register file.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/cards/card.hpp"
#include "core/cards/card256_storage.hpp"
#include "core/cards/vsid.hpp"

namespace freetvc {

class Card256 final : public Card, public VideoOverlay {
public:
    using Usb = Card256Usb;

    static constexpr std::size_t kRomSize = 0x3000;
    static constexpr std::size_t kRomBankSize = 0x1800;
    // The MCU flash's power-on pictures: the font, then the bitmap's pixel rows as stored in its BMP (bottom row first)
    static constexpr std::size_t kFontSize = 0x800;
    static constexpr std::size_t kPictureSize = 128 * 240;
    static constexpr std::size_t kPictureHeader = 118;  // the BMP's header and palette, skipped by the firmware
    static constexpr std::size_t kImageSize = kRomSize + kFontSize + kPictureSize;
    static constexpr std::size_t kPageSize = 0x4000;
    static constexpr unsigned kFastPages = 16;
    static constexpr unsigned kWindowPages = 2;  // pages 16/17 hold the PSRAM page mapped at U2/U3
    static constexpr std::size_t kFastSize = (kFastPages + kWindowPages) * kPageSize;
    static constexpr unsigned kPsramPages = 512;
    static constexpr std::size_t kPsramSize = kPsramPages * kPageSize;
    static constexpr std::uint8_t kVersionMajor = 0x00;  // firmware 0.24
    static constexpr std::uint8_t kVersionMinor = 24;

    // The 12 KiB ROM, optionally followed by the font and the picture (kImageSize); a missing ROM part reads FFh, a missing picture part 0.
    explicit Card256(std::span<const std::uint8_t> image);

    std::unique_ptr<Card> clone() const override { return std::make_unique<Card256>(*this); }
    CardKind kind() const override { return CardKind::Card256; }
    unsigned id() const override { return 3; }  // IDA_N/IDB_N not connected: named by its ROM header
    void power_on() override;
    void reset(std::uint64_t tick) override;
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t offset) const override { return iomem_[offset & 0x1FFF]; }
    void mem_write(std::uint16_t offset, std::uint8_t value) override;
    std::span<std::uint8_t> window() override { return iomem_; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;
    std::uint8_t* phantom_page(unsigned unit) override;
    void border_write(std::uint8_t value) override;
    // The vSID on the bus SOUND line
    void advance_audio(std::uint64_t tick, AudioSynth& out) override;
    VideoOverlay* overlay() override { return this; }
    bool irq() const override { return irq_; }
    void video_character(bool display, bool vsync, std::uint8_t mode, std::uint8_t* px) override;

    // A register as the data port would read it, without the read's side effects
    std::uint8_t peek_register(std::uint8_t index) const;
    unsigned rom_bank() const { return rom_bank_; }
    // B0h/B1h: the fast-RAM page at U2/U3, FFh for the machine's own RAM
    std::uint8_t unit_page(unsigned unit) const { return unit_page_[unit & 1]; }
    std::span<std::uint8_t> fast_ram() { return fast_; }
    std::span<const std::uint8_t> fast_ram() const { return fast_; }
    std::uint8_t psram_read(std::uint32_t address) const;
    void psram_write(std::uint32_t address, std::uint8_t value);
    // PSRAM pages holding anything but zeros
    unsigned psram_pages_used() const;

    // What is plugged into the USB port; with Card256Media attached the card takes its `usb` at every access
    void set_usb(Usb device);
    Usb usb() const { return usb_; }
    // A HID boot-protocol mouse report: movement since the last one (y down) and buttons (b0 left, b1 right, b2 middle)
    void mouse_report(int dx, int dy, std::uint8_t buttons);
    std::uint8_t dsk_kind() const { return dsk_type_; }  // 0 none, 1 on the pendrive, 2 PSRAM, 3 flash
    const std::string& dsk_path() const { return dsk_path_; }
    bool psram_drive() const { return psram_drive_; }

private:
    using PsramPage = std::array<std::uint8_t, kPageSize>;
    struct Sprite {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint8_t colour = 0;
        std::uint8_t phase = 0;
        std::uint8_t foreground = 0;
        std::uint8_t enabled = 0;
        std::uint8_t full_colour = 0;
    };

    // The overlay core's states ([FW] tvc-graphics.c draw_screen_cycle): syncing after a start, waiting for VSYNC, drawing lines
    enum class OverlayState : std::uint8_t { Sync, WaitVsync, Frame };
    static constexpr std::size_t kLineBuffer = 256 + 48;  // 24 pixels each side for sprites ([FW] extc_bits_line)
    static constexpr std::uint8_t kClear = 0x10;          // /EXTC high: the TVC's own pixel

    void follow_media(const CardBus& bus);
    // The vSID's clock (336 MHz / 341) at a machine tick (6.25 MHz) since the MCU started, and back (rounded up)
    std::uint64_t vsid_clock(std::uint64_t tick) const { return (tick - std::min(tick, mcu_start_)) * 1344 / 8525; }
    std::uint64_t vsid_tick(std::uint64_t clock) const { return mcu_start_ + (clock * 8525 + 1343) / 1344; }
    std::uint8_t read_register(std::uint8_t index);
    void write_register(std::uint8_t index, std::uint8_t value, const CardBus& bus);
    void rebuild_sprite_words();
    void load_pictures();  // card256_overlay.cpp
    void overlay_power_on();
    void vsync_end();
    void paper_start();
    void compose_line(std::array<std::uint8_t, 256>& out);
    void draw_sprites(std::array<std::uint8_t, kLineBuffer>& buf, std::array<std::uint16_t, kLineBuffer>& hits, std::uint16_t& sum,
                      bool front) const;
    std::uint16_t background_hits() const;
    void select_rom_bank(unsigned bank);
    void map_psram(unsigned unit, std::uint8_t page);
    std::uint8_t* psram_page_for_write(unsigned page);

    // Register C0h/C6h functions (card256_functions.cpp); `at` is the parameter block's fast-RAM offset
    std::uint8_t call(std::uint8_t code, std::uint32_t at, const CardBus& bus);
    std::uint32_t param_size(std::uint8_t code, std::uint32_t at) const;
    void run_function(std::uint8_t code, const CardBus& bus);
    void run_chain(std::uint8_t count, const CardBus& bus);
    std::uint8_t fast_get(std::uint32_t address) const { return address < fast_.size() ? fast_[address] : 0; }
    void fast_put(std::uint32_t address, std::uint8_t value) {
        if (address < fast_.size()) {
            fast_[address] = value;
        }
    }
    // A 24-bit function address: b23 = PSRAM, else fast RAM
    std::uint8_t mem_get(std::uint32_t address) const;
    void mem_put(std::uint32_t address, std::uint8_t value);
    std::uint16_t param16(std::uint32_t at) const;
    std::uint32_t param24(std::uint32_t at) const;
    std::uint32_t param32(std::uint32_t at) const;
    void fast_move(std::uint32_t dst, std::uint32_t src, std::uint32_t length);
    std::uint8_t move_short(std::uint32_t at);
    std::uint8_t move_full(std::uint32_t at);
    std::uint8_t move_from_slow(std::uint32_t at);
    std::uint8_t move_to_slow(std::uint32_t at);
    std::uint8_t move_chunks_from_block(std::uint32_t at);
    std::uint8_t move_chunks(std::uint32_t dst, std::uint32_t src, std::uint16_t chunk, std::uint16_t count, std::uint16_t dst_step,
                             std::uint16_t src_step);
    std::uint8_t mirror_sprite_phase(std::uint32_t at);
    std::uint8_t zx7(std::uint32_t at);
    std::uint8_t evaluate_math(std::uint32_t at);

    // Storage (card256_storage.cpp): the D6h commands on card RAM D900h/DA00h, the C0h file functions 81h-93h, 15, 34-36
    class Session;
    struct FatFile {
        bool used = false;  // the pool slot ([FW] availableFileHandles)
        bool open = false;
        std::uint8_t volume = 0;  // FatFs drive: 0 the pendrive, 1 the DSK
        std::string path;
        std::uint64_t pos = 0;
        std::uint8_t mode = 0;  // FatFs FA_ bits
    };
    struct FatDir {
        bool used = false;
        bool open = false;
        std::uint8_t volume = 0;
        std::string path;
        std::uint32_t index = 0;
    };
    struct LfsFile {
        bool used = false;  // the pool slot ([FW] lfs_file_in_use)
        bool open = false;
        std::uint8_t drive = 0;  // 1 PSRAM, 2 flash
        std::string path;
        int flags = 0;  // littlefs flags for reopening
        std::uint32_t pos = 0;
    };
    struct LfsDir {
        bool used = false;
        std::uint8_t drive = 0;
        std::string path;
        std::uint32_t index = 0;
    };
    std::uint8_t storage_command(std::uint8_t code, const CardBus& bus);
    std::uint8_t storage_dispatch(Session& s, std::uint8_t code);
    std::optional<std::uint8_t> call_file_function(std::uint8_t code, std::uint32_t at, const CardBus& bus);
    std::uint8_t copy_dir_to_psram(Session& s, std::uint32_t at);
    std::uint8_t create_psram_drive(Session& s, std::uint32_t at);
    std::uint8_t delete_psram_drive();
    int psram_drive_init(Session& s, std::uint32_t blocks);
    void psram_fill(std::uint32_t pos, std::uint32_t n, std::uint8_t value);
    void dsk_unmounted();
    // [FW] TVC_ROM: the card's IOMEM, card RAM from 1800h; the firmware's overruns past it are dropped
    std::uint8_t rom(unsigned offset) const { return offset < iomem_.size() ? iomem_[offset] : 0; }
    void set_rom(unsigned offset, std::uint8_t value) {
        if (offset < iomem_.size()) {
            iomem_[offset] = value;
        }
    }
    void lower_rom(unsigned offset);  // [FW] str_tolower in place
    std::uint32_t rom32(unsigned offset) const;
    void set_rom32(unsigned offset, std::uint32_t value);
    std::string rom_string(unsigned offset, unsigned length) const;
    void put_rom_string(unsigned offset, const std::string& text);
    // The FatFs layer ([FW] tvc-fatfs.c) on the pendrive (volume 0) and the mounted DSK (volume 1)
    std::uint8_t fat_open(Session& s, std::uint8_t volume);
    std::uint8_t fat_close(Session& s);
    std::uint8_t fat_read(Session& s);
    std::uint8_t fat_read_dest(Session& s);
    std::uint8_t fat_write(Session& s);
    std::uint8_t fat_write_source(Session& s);
    std::uint8_t fat_open_dir(Session& s, std::uint8_t volume);
    std::uint8_t fat_close_dir(Session& s);
    std::uint8_t fat_read_dir(Session& s);
    std::uint8_t fat_seek(Session& s);
    std::uint8_t fat_getcwd(Session& s, std::uint8_t volume);
    std::uint8_t fat_chdir(Session& s, std::uint8_t volume);
    std::uint8_t fat_mkdir(Session& s, std::uint8_t volume);
    std::uint8_t fat_delete(Session& s, std::uint8_t volume);
    std::uint8_t fat_rename(Session& s, std::uint8_t volume);
    std::uint8_t fat_stat(Session& s, std::uint8_t volume);
    std::uint8_t fat_sync(Session& s);
    std::uint8_t fat_mount_dsk(Session& s);
    std::uint8_t fat_unmount_dsk(Session& s);
    void put_filinfo(const FatEntry& e);
    // The littlefs layer ([FW] tvc-lfs.c) on the PSRAM drive (1) and the flash drive (2)
    std::uint8_t lfs_open(Session& s, std::uint8_t drive);
    std::uint8_t lfs_close(Session& s);
    std::uint8_t lfs_read(Session& s);
    std::uint8_t lfs_write(Session& s);
    std::uint8_t lfs_open_dir(Session& s, std::uint8_t drive);
    std::uint8_t lfs_close_dir(Session& s);
    std::uint8_t lfs_read_dir(Session& s);
    std::uint8_t lfs_seek(Session& s);
    std::uint8_t lfs_read_dest(Session& s);
    std::uint8_t lfs_write_source(Session& s);
    std::uint8_t lfs_getcwd(std::uint8_t drive);
    std::uint8_t lfs_chdir(Session& s, std::uint8_t drive);
    std::uint8_t lfs_mkdir(Session& s, std::uint8_t drive);
    std::uint8_t lfs_delete(Session& s, std::uint8_t drive);
    std::uint8_t lfs_stat(Session& s, std::uint8_t drive);
    std::uint8_t lfs_sync(Session& s);
    std::uint8_t lfs_mount_dsk(Session& s, std::uint8_t drive);
    std::uint8_t lfs_unmount_dsk(Session& s);
    std::string& lfs_cwd(std::uint8_t drive) { return drive == 1 ? psram_cwd_ : flash_cwd_; }

    // Text and drawing functions (card256_graphics.cpp); drawing goes to the bitmap select_draw_bitmap() picks
    std::optional<std::uint8_t> call_graphics(std::uint8_t code, std::uint32_t at);
    std::int16_t param_s16(std::uint32_t at) const;
    void select_draw_bitmap();
    void set_dot(std::uint8_t x, std::uint8_t y, std::uint8_t colour);
    std::uint8_t get_dot(std::uint8_t x, std::uint8_t y) const;
    void draw_line(std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2, std::uint8_t colour);
    void draw_hline(std::int16_t x1, std::int16_t x2, std::int16_t y, std::uint8_t colour);
    void draw_hline_2c(std::int16_t x1, std::int16_t x2, std::int16_t y, std::uint8_t two);
    void fix_rect(std::uint32_t at, std::int16_t& x, std::int16_t& y, std::uint8_t& w, std::uint8_t& h) const;
    std::uint8_t draw_rectangle(std::uint32_t at);
    std::uint8_t flood_fill(std::uint8_t start_x, std::uint8_t start_y, std::uint8_t colour, bool two_colours);
    std::uint8_t ellipse(std::uint32_t at, bool fill);
    std::uint8_t copy_image_block(std::uint32_t at);
    std::uint8_t copy_sub_image(std::uint32_t at);
    std::uint8_t fill_triangle(std::uint32_t at);
    void scroll_text_up();
    void print_code(std::uint8_t code);
    void print_hex_digit(unsigned digit);
    void print_ascii(std::uint32_t at);
    void print_ascii_char(std::uint8_t c);
    void print_ascii_text(const std::string& text);

    std::array<std::uint8_t, kRomSize> rom_{};
    std::shared_ptr<const std::vector<std::uint8_t>> pictures_;  // font and picture, kFontSize + kPictureSize; null = none
    std::array<std::uint8_t, 0x2000> iomem_{};  // C000-D7FF the ROM bank, D800-DFFF card RAM
    std::vector<std::uint8_t> fast_ = std::vector<std::uint8_t>(kFastSize);
    std::array<std::shared_ptr<PsramPage>, kPsramPages> psram_{};  // shared between copies until written; null = zeros
    unsigned rom_bank_ = 0;
    std::array<std::uint8_t, 2> select_{};
    std::array<std::uint8_t, 2> increment_{};
    std::array<Sprite, 16> sprites_{};
    std::uint16_t enabled_word_ = 0;
    std::uint16_t foreground_word_ = 0;
    std::uint16_t colour_mode_word_ = 0;
    std::uint16_t sp_sp_collision_ = 0;
    std::uint16_t sp_bg_collision_ = 0;
    std::uint16_t sp_sp_mask_ = 0;
    std::uint16_t sp_bg_mask_ = 0;
    std::uint8_t sprite_base_ = 0;
    std::uint8_t video_mode_ = 0;
    std::uint8_t screen_base_ = 0;
    std::uint8_t colour_base_ = 0;
    std::uint8_t font_base_ = 0;
    std::uint8_t bitmap_base_ = 0;
    std::uint8_t scroll_x_ = 0;
    std::uint8_t scroll_y_ = 0;
    std::uint8_t border_ = 0;
    std::uint16_t line_ = 0;      // the overlay's line counter (A8h)
    std::uint16_t max_line_ = 0;  // last line + 1, 21 lines above the picture
    std::uint16_t screen_height_ = 0;
    std::array<std::uint8_t, 2> unit_page_{};
    std::array<std::uint16_t, 2> psram_mapped_{};
    std::uint8_t function_ = 0;
    std::uint8_t param_start_ = 0;
    std::uint8_t result_ = 0;
    std::uint8_t chain_ = 0;
    std::uint8_t function_bitmap_base_ = 0;
    std::uint8_t mouse_speed_ = 0x10;  // in the card's flash: kept across power cycles
    std::uint8_t storage_status_ = 0;
    std::uint8_t debug_ = 0;
    VSid vsid_;
    std::uint64_t mcu_start_ = 0;  // the tick the MCU last started: the vSID's clock counts from it
    float vsid_level_ = 0.0f;      // the level last given to the synth
    bool vsid_drop_ = false;       // a reboot cut the output: the synth goes back to 0
    std::uint64_t last_start_ = 0;  // the MCU's last (re)start, for the double reset

    // The overlay (card256_overlay.cpp); IRGB pixels, kClear where the TVC shows through
    OverlayState overlay_state_ = OverlayState::Sync;
    std::uint8_t sync_vsyncs_ = 0;  // VSYNC ends seen while syncing
    bool restart_ = false;          // the reset ends the frame and syncs again
    bool last_display_ = false;
    bool last_vsync_ = false;
    unsigned paper_px_ = 0;  // overlay pixels since this paper line started
    std::array<std::uint8_t, 256> shown_{};
    std::array<std::uint8_t, 256> next_{};
    std::uint8_t held_ = kClear;  // what the PIO's pins keep after a line's 256 pixels
    std::array<std::uint8_t, kLineBuffer> bg_mask_{};  // [FW] sprite_background_line_mask, never cleared whole
    bool irq_ = false;

    // The text functions' cursor and colour ([FW] tvc-routines.c screenTextPosX/Y, screenTextColor)
    std::uint8_t text_x_ = 0;
    std::uint8_t text_y_ = 0;
    std::uint8_t text_colour_ = 0x0F;
    std::uint8_t pen_colour_ = 0;
    std::uint32_t draw_base_ = 0;  // [FW] functionBitmapBaseAddr, set by each drawing function

    // Storage: the firmware's handle pools, working directories and drives (card256_storage.cpp)
    std::array<FatFile, 4> fat_files_{};
    std::array<FatDir, 4> fat_dirs_{};
    std::array<LfsFile, 4> lfs_files_{};
    std::array<LfsDir, 4> lfs_dirs_{};
    std::array<std::string, 2> fat_cwd_{"/", "/"};
    std::uint8_t fat_drive_ = 0;  // FatFs's current drive ([FW] activeDrive)
    std::string psram_cwd_ = "/";
    std::string flash_cwd_ = "/";
    std::string temp_path_;  // [FW] temp_path, which some length checks read stale
    std::uint8_t dsk_type_ = 0;
    std::string dsk_path_;
    bool psram_drive_ = false;
    std::uint32_t psram_blocks_ = 0;
    std::uint32_t free_psram_start_ = 0;
    bool flash_ok_ = false;  // the flash drive mounted, seen at the last access
    Usb usb_ = Usb::None;

    // The mouse ([FW] tvc-usb.c process_mouse_report)
    std::uint8_t mouse_buttons_ = 0;
    std::int32_t mouse_dx_sum_ = 0;
    std::int32_t mouse_dy_sum_ = 0;
    std::uint8_t mouse_dx_ = 0;
    std::uint8_t mouse_dy_ = 0;
    bool mouse_dx_read_ = false;  // a read since the last report restarts the sum
    bool mouse_dy_read_ = false;
};

}  // namespace freetvc
