// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/card256.hpp"

#include <algorithm>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint16_t kNoPsram = 0xFFFF;
constexpr std::uint16_t kLinesAbove = 21;  // the overlay's lines above the picture ([FW] tvc-graphics.c:71-72)
constexpr std::uint8_t kSaveVersion = 4;  // 2: the overlay; 3: storage, USB port and mouse; 4: the vSID

}  // namespace

Card256::Card256(std::span<const std::uint8_t> image) {
    rom_.fill(0xFF);
    std::copy_n(image.begin(), std::min(image.size(), rom_.size()), rom_.begin());
    if (image.size() > kRomSize) {
        auto pictures = std::make_shared<std::vector<std::uint8_t>>(kFontSize + kPictureSize, std::uint8_t{0});
        std::copy_n(image.begin() + kRomSize, std::min(image.size() - kRomSize, pictures->size()), pictures->begin());
        pictures_ = std::move(pictures);
    }
    power_on();
}

// The MCU's boot ([FW] TVC-IO-main-sys.c:61-100, initRegisterPointers; tvc-graphics.c init_graphics)
void Card256::power_on() {
    std::fill(fast_.begin(), fast_.end(), std::uint8_t{0});
    psram_.fill(nullptr);
    std::fill(iomem_.begin(), iomem_.end(), std::uint8_t{0});
    select_rom_bank(0);
    select_ = {0, 0};
    increment_ = {1, 1};
    for (unsigned i = 0; i < sprites_.size(); ++i) {
        sprites_[i] = {static_cast<std::uint16_t>(i * 12 + 24), kLinesAbove, 15, 0x40, 1, 0, 1};
    }
    rebuild_sprite_words();
    sp_sp_collision_ = sp_bg_collision_ = sp_sp_mask_ = sp_bg_mask_ = 0;
    sprite_base_ = 0;
    video_mode_ = 0;
    screen_base_ = 1;
    colour_base_ = 2;
    font_base_ = 2;
    bitmap_base_ = 1;
    scroll_x_ = scroll_y_ = 0;
    border_ = 0;
    screen_height_ = 240;
    max_line_ = screen_height_ + kLinesAbove;
    line_ = kLinesAbove;
    unit_page_ = {4, 5};
    psram_mapped_ = {kNoPsram, kNoPsram};
    function_ = param_start_ = result_ = chain_ = 0;
    function_bitmap_base_ = 0xFF;
    storage_status_ = 0;
    debug_ = 0;
    vsid_.power_on();
    mcu_start_ = 0;
    vsid_level_ = 0.0f;
    vsid_drop_ = false;
    last_start_ = 0;
    // The MCU's storage starts empty; what is plugged into the USB port stays
    fat_files_ = {};
    fat_dirs_ = {};
    lfs_files_ = {};
    lfs_dirs_ = {};
    fat_cwd_ = {"/", "/"};
    fat_drive_ = 0;
    psram_cwd_ = flash_cwd_ = "/";
    temp_path_.clear();
    dsk_type_ = 0;
    dsk_path_.clear();
    psram_drive_ = false;
    psram_blocks_ = 0;
    free_psram_start_ = 0;
    mouse_buttons_ = mouse_dx_ = mouse_dy_ = 0;
    mouse_dx_sum_ = mouse_dy_sum_ = 0;
    mouse_dx_read_ = mouse_dy_read_ = false;
    overlay_power_on();
}

// One press: a warm reset of the overlay; a second within a second reboots the MCU ([FW] tvc-graphics.c reset_handler)
void Card256::reset(std::uint64_t tick) {
    if (tick - std::min(tick, last_start_) <= AudioSynth::kTickRate) {
        const float level = vsid_level_;
        power_on();
        last_start_ = mcu_start_ = tick;
        vsid_level_ = level;  // the synth still holds it until advance_audio() takes it back
        vsid_drop_ = true;
        return;
    }
    last_start_ = tick;
    // The overlay core leaves its frame (at once, or after the one it is waiting for) and syncs again
    if (overlay_state_ == OverlayState::Sync) {
        sync_vsyncs_ = 0;
    } else {
        restart_ = true;
    }
    video_mode_ = 0;
    line_ = max_line_;
    scroll_x_ = scroll_y_ = 0;
    for (Sprite& s : sprites_) {
        s.enabled = 0;
    }
    enabled_word_ = 0;
    // Only the read-back register: the engine keeps its volume, so the sound goes on (⚠ the manual says it silences)
    vsid_.set_reg(24, 0);
}

void Card256::advance_audio(std::uint64_t tick, AudioSynth& out) {
    if (vsid_drop_) {
        if (vsid_level_ != 0.0f) {
            out.add_step(mcu_start_, -vsid_level_);
        }
        vsid_level_ = 0.0f;
        vsid_drop_ = false;
    }
    // The two PWM outputs' 16-bit value, full scale like the TVC's own tone at amplitude 15 (❓)
    constexpr float kLevel = 0.75f / 65535.0f;
    vsid_.compute_to(vsid_clock(tick));
    while (vsid_.has_output()) {
        const std::uint64_t at = vsid_tick(vsid_.output_clock());
        if (at > tick) {
            break;
        }
        const float level = static_cast<float>(vsid_.output_value()) * kLevel;
        if (level != vsid_level_) {
            out.add_step(at, level - vsid_level_);
            vsid_level_ = level;
        }
        vsid_.pop_output();
    }
}

void Card256::select_rom_bank(unsigned bank) {
    rom_bank_ = bank & 1;
    std::copy_n(rom_.begin() + static_cast<std::ptrdiff_t>(rom_bank_ * kRomBankSize), kRomBankSize, iomem_.begin());
}

void Card256::mem_write(std::uint16_t offset, std::uint8_t value) {
    offset &= 0x1FFF;
    if (offset >= kRomBankSize) {
        iomem_[offset] = value;
    }
}

std::uint8_t* Card256::phantom_page(unsigned unit) {
    if (unit < 2 || unit > 3) {
        return nullptr;
    }
    const std::uint8_t page = unit_page_[unit - 2];
    if (page == 0xFF) {
        return nullptr;
    }
    return fast_.data() + static_cast<std::size_t>(page) * kPageSize;
}

// IGRB in b7, b5, b3, b1 of port 00h becomes IRGB ([FW] irq_handler_border_io)
void Card256::border_write(std::uint8_t value) {
    border_ = static_cast<std::uint8_t>(((value >> 1) & 0x01) | ((value >> 1) & 0x04) | ((value >> 4) & 0x02) | ((value >> 4) & 0x08));
}

// Only A0-A2 are decoded: +0/+4 index, +1/+5 data, +2/+6 increment, +3/+7 nothing ([FW] irq_handler_slotn)
std::uint8_t Card256::io_read(unsigned port, const CardBus& bus) {
    const unsigned set = (port >> 2) & 1;
    switch (port & 3) {
    case 0:
        return select_[set];
    case 1: {
        const std::uint8_t index = select_[set];
        follow_media(bus);
        select_[set] = static_cast<std::uint8_t>(select_[set] + increment_[set]);
        if (index >= 0xE0) {
            vsid_.compute_to(vsid_clock(bus.tick));  // the registers as the blocks computed so far left them
        }
        return read_register(index);
    }
    case 2:
        return increment_[set];
    default:
        return 0x00;
    }
}

void Card256::io_write(unsigned port, std::uint8_t value, const CardBus& bus) {
    const unsigned set = (port >> 2) & 1;
    switch (port & 3) {
    case 0:
        select_[set] = value;
        break;
    case 1: {
        const std::uint8_t index = select_[set];
        select_[set] = static_cast<std::uint8_t>(select_[set] + increment_[set]);
        follow_media(bus);
        write_register(index, value, bus);
        break;
    }
    case 2:
        increment_[set] = value;
        break;
    default:
        break;
    }
}

// The plug and the flash drive as the front end has them (outside the machine state)
void Card256::follow_media(const CardBus& bus) {
    if (bus.card256) {
        set_usb(bus.card256->usb);
        flash_ok_ = bus.card256->flash && bus.card256->flash_valid;
    }
}

std::uint8_t Card256::peek_register(std::uint8_t index) const {
    const auto lo = [](std::uint16_t v) { return static_cast<std::uint8_t>(v); };
    const auto hi = [](std::uint16_t v) { return static_cast<std::uint8_t>(v >> 8); };
    const auto half = [&](std::uint16_t v) { return (index & 1) ? hi(v) : lo(v); };
    if (index < 0x20) {
        return half(sprites_[index >> 1].x);
    }
    if (index < 0x40) {
        return half(sprites_[(index - 0x20) >> 1].y);
    }
    if (index < 0x90) {
        const Sprite& s = sprites_[index & 0x0F];
        switch (index >> 4) {
        case 0x4:
            return s.colour;
        case 0x5:
            return s.phase;
        case 0x6:
            return s.foreground;
        case 0x7:
            return s.enabled;
        default:
            return s.full_colour;
        }
    }
    if (index >= 0xE0) {
        return vsid_.reg(index - 0xE0u);
    }
    switch (index) {
    case 0x90:
    case 0x91:
        return half(enabled_word_);
    case 0x92:
    case 0x93:
        return half(foreground_word_);
    case 0x94:
    case 0x95:
        return half(colour_mode_word_);
    case 0x96:
    case 0x97:
        return half(sp_sp_collision_);
    case 0x98:
    case 0x99:
        return half(sp_bg_collision_);
    case 0x9A:
    case 0x9B:
        return half(sp_sp_mask_);
    case 0x9C:
    case 0x9D:
        return half(sp_bg_mask_);
    case 0x9E:
        return sprite_base_;
    case 0xA0:
        return video_mode_;
    case 0xA1:
        return screen_base_;
    case 0xA2:
        return colour_base_;
    case 0xA3:
        return font_base_;
    case 0xA4:
        return bitmap_base_;
    case 0xA5:
        return scroll_x_;
    case 0xA6:
        return scroll_y_;
    case 0xA7:
        return border_;
    case 0xA8:
        return lo(line_);
    case 0xA9:
        return lo(screen_height_);
    case 0xB0:
    case 0xB1:
        return unit_page_[index & 1];
    case 0xB2:
    case 0xB3:
        return (index & 1) ? hi(psram_mapped_[0]) : lo(psram_mapped_[0]);
    case 0xB4:
    case 0xB5:
        return (index & 1) ? hi(psram_mapped_[1]) : lo(psram_mapped_[1]);
    case 0xB6:
        return static_cast<std::uint8_t>(kPsramSize >> 20);
    case 0xB7:
        return static_cast<std::uint8_t>(rom_bank_);
    case 0xC0:
        return function_;
    case 0xC1:
        return param_start_;
    case 0xC2:
        return result_;
    case 0xC3:
        return kVersionMajor;
    case 0xC4:
        return kVersionMinor;
    case 0xC5:
        return iomem_[0x1810];  // the ROM's selected drive, D810h
    case 0xC6:
        return chain_;
    case 0xC7:
        return function_bitmap_base_;
    case 0xC8:
        return iomem_[0x1824];  // D824h: PSRAM drive valid
    case 0xC9:
        return flash_ok_;  // [FW] flash_drive_initialized
    case 0xCA:
        return iomem_[0x1825];  // D825h: DSK drive valid
    case 0xD0:
        return static_cast<std::uint8_t>((usb_ == Usb::Mouse ? 0x01 : 0) | (usb_ == Usb::Pendrive ? 0x02 : 0));
    case 0xD1:
        return mouse_buttons_;
    case 0xD2:
        return mouse_dx_;
    case 0xD3:
        return mouse_dy_;
    case 0xD5:
        return mouse_speed_;
    case 0xD6:
        return storage_status_;
    case 0xDC:
        return debug_;
    default:
        return 0x00;  // the wheel (D4h, never filled) and every register without a read place
    }
}

std::uint8_t Card256::read_register(std::uint8_t index) {
    const std::uint8_t value = peek_register(index);
    // Reading the low byte of a collision word clears the word and releases the slot interrupt ([FW] :306-314)
    if (index == 0x96) {
        sp_sp_collision_ = 0;
        irq_ = false;
    } else if (index == 0x98) {
        sp_bg_collision_ = 0;
        irq_ = false;
    } else if (index == 0xD2) {
        mouse_dx_ = 0;  // the next report starts a new sum ([FW] mouseUpdateXID)
        mouse_dx_read_ = true;
    } else if (index == 0xD3) {
        mouse_dy_ = 0;
        mouse_dy_read_ = true;
    }
    return value;
}

void Card256::rebuild_sprite_words() {
    enabled_word_ = foreground_word_ = colour_mode_word_ = 0;
    for (unsigned i = 0; i < sprites_.size(); ++i) {
        Sprite& s = sprites_[i];
        s.enabled = s.enabled != 0;
        s.foreground = s.foreground != 0;
        s.full_colour = s.full_colour != 0;
        enabled_word_ |= static_cast<std::uint16_t>(s.enabled << i);
        foreground_word_ |= static_cast<std::uint16_t>(s.foreground << i);
        colour_mode_word_ |= static_cast<std::uint16_t>(s.full_colour << i);
    }
}

// The write lands in the register's place, then the firmware's main loop follows up ([FW] process_queue_cycle)
void Card256::write_register(std::uint8_t index, std::uint8_t value, const CardBus& bus) {
    const auto set_half = [&](std::uint16_t& word) {
        word = (index & 1) ? static_cast<std::uint16_t>((word & 0x00FF) | (value << 8))
                           : static_cast<std::uint16_t>((word & 0xFF00) | value);
    };
    if (index < 0x20) {
        set_half(sprites_[index >> 1].x);
        return;
    }
    if (index < 0x40) {
        set_half(sprites_[(index - 0x20) >> 1].y);
        return;
    }
    if (index < 0x90) {
        Sprite& s = sprites_[index & 0x0F];
        switch (index >> 4) {
        case 0x4:
            s.colour = value;
            return;
        case 0x5:
            s.phase = value;
            return;
        case 0x6:
            s.foreground = value;
            break;
        case 0x7:
            s.enabled = value;
            break;
        default:
            s.full_colour = value;
            break;
        }
        rebuild_sprite_words();
        return;
    }
    if (index >= 0xE0) {
        // Queued with its vSID clock; the read-back changes only when its block is computed ([FW] dummySIDWriteRegister)
        vsid_.queue_write(static_cast<std::uint8_t>(index - 0xE0), value, vsid_clock(bus.tick));
        return;
    }
    switch (index) {
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95: {
        const unsigned first = (index & 1) * 8;
        for (unsigned i = 0; i < 8; ++i) {
            const std::uint8_t bit = (value >> i) & 1;
            Sprite& s = sprites_[first + i];
            (index < 0x92 ? s.enabled : index < 0x94 ? s.foreground : s.full_colour) = bit;
        }
        set_half(index < 0x92 ? enabled_word_ : index < 0x94 ? foreground_word_ : colour_mode_word_);
        break;
    }
    case 0x9A:
    case 0x9B:
        set_half(sp_sp_mask_);
        break;
    case 0x9C:
    case 0x9D:
        set_half(sp_bg_mask_);
        break;
    case 0x9E:
        sprite_base_ = value & 0x07;
        break;
    case 0xA0:
        video_mode_ = value & 0x03;
        break;
    case 0xA1:
        screen_base_ = value & 0x7F;
        break;
    case 0xA2:
        colour_base_ = value & 0x7F;
        break;
    case 0xA3:
        font_base_ = value & 0x3F;
        break;
    case 0xA4:
        bitmap_base_ = value & 0x03;
        break;
    case 0xA5:
        scroll_x_ = value & 0x87;
        break;
    case 0xA6:
        scroll_y_ = value & 0x87;
        break;
    case 0xA7:
        border_ = value & 0x0F;
        break;
    case 0xA9:
        if (value > 1 && value < 240) {
            screen_height_ = value;
            max_line_ = static_cast<std::uint16_t>(value + kLinesAbove);
        }
        break;
    case 0xB0:
    case 0xB1:
        unit_page_[index & 1] = value == 0xFF ? 0xFF : value & 0x0F;
        break;
    case 0xB2:
        map_psram(0, value);
        break;
    case 0xB4:
        map_psram(1, value);
        break;
    case 0xB7:
        if (value < 2) {
            select_rom_bank(value);
        }
        break;
    case 0xC0:
        function_ = value;
        run_function(value, bus);
        function_ = 0xFF;
        break;
    case 0xC1:
        param_start_ = value;
        break;
    case 0xC2:
        result_ = value;
        break;
    case 0xC5:
        iomem_[0x1810] = std::min<std::uint8_t>(value, 3);
        break;
    case 0xC6:
        chain_ = value;
        run_chain(value, bus);
        chain_ = 0xFF;
        break;
    case 0xC7:
        function_bitmap_base_ = value == 0xFF ? 0xFF : value & 0x03;
        break;
    case 0xD5:
        mouse_speed_ = value & 0x7F;
        break;
    case 0xD6:
        // The command runs at once; the card shows its code while busy, which the Z80 never sees here
        storage_status_ = storage_command(value, bus);
        break;
    case 0xDC:
        debug_ = value;
        break;
    default:
        break;  // read-only and unused registers
    }
}

// Back to PSRAM what the window held, then the new page in ([FW] processPSRAMMapping, 675ff.)
void Card256::map_psram(unsigned unit, std::uint8_t page) {
    std::uint8_t* window = fast_.data() + static_cast<std::size_t>(kFastPages + unit) * kPageSize;
    if (psram_mapped_[unit] != page) {
        if (psram_mapped_[unit] != kNoPsram) {
            std::copy_n(window, kPageSize, psram_page_for_write(psram_mapped_[unit]));
        }
        if (const auto& from = psram_[page]) {
            std::copy_n(from->begin(), kPageSize, window);
        } else {
            std::fill_n(window, kPageSize, std::uint8_t{0});
        }
        psram_mapped_[unit] = page;
    }
    unit_page_[unit] = static_cast<std::uint8_t>(kFastPages + unit);
}

std::uint8_t* Card256::psram_page_for_write(unsigned page) {
    std::shared_ptr<PsramPage>& p = psram_[page % kPsramPages];
    if (!p) {
        p = std::make_shared<PsramPage>();
        p->fill(0);
    } else if (p.use_count() > 1) {
        p = std::make_shared<PsramPage>(*p);
    }
    return p->data();
}

std::uint8_t Card256::psram_read(std::uint32_t address) const {
    if (address >= kPsramSize) {
        return 0;
    }
    const auto& p = psram_[address / kPageSize];
    return p ? (*p)[address % kPageSize] : 0;
}

void Card256::psram_write(std::uint32_t address, std::uint8_t value) {
    if (address >= kPsramSize) {
        return;
    }
    const auto& p = psram_[address / kPageSize];
    if (!p && value == 0) {
        return;
    }
    psram_page_for_write(address / kPageSize)[address % kPageSize] = value;
}

unsigned Card256::psram_pages_used() const {
    unsigned n = 0;
    for (const auto& p : psram_) {
        if (p && std::any_of(p->begin(), p->end(), [](std::uint8_t b) { return b != 0; })) {
            ++n;
        }
    }
    return n;
}

void Card256::save(ChunkWriter& w) const {
    w.u8(kSaveVersion);
    w.u8(static_cast<std::uint8_t>(rom_bank_));
    w.packed(std::span<const std::uint8_t>(iomem_).subspan(kRomBankSize));
    w.packed(fast_);
    w.u16(static_cast<std::uint16_t>(psram_pages_used()));
    for (unsigned i = 0; i < kPsramPages; ++i) {
        const auto& p = psram_[i];
        if (p && std::any_of(p->begin(), p->end(), [](std::uint8_t b) { return b != 0; })) {
            w.u16(static_cast<std::uint16_t>(i));
            w.packed(*p);
        }
    }
    for (unsigned set = 0; set < 2; ++set) {
        w.u8(select_[set]);
        w.u8(increment_[set]);
        w.u8(unit_page_[set]);
        w.u16(psram_mapped_[set]);
    }
    for (const Sprite& s : sprites_) {
        w.u16(s.x);
        w.u16(s.y);
        w.u8(s.colour);
        w.u8(s.phase);
        w.u8(s.foreground);
        w.u8(s.enabled);
        w.u8(s.full_colour);
    }
    for (std::uint16_t v : {sp_sp_collision_, sp_bg_collision_, sp_sp_mask_, sp_bg_mask_, line_, max_line_, screen_height_}) {
        w.u16(v);
    }
    for (std::uint8_t v : {sprite_base_, video_mode_, screen_base_, colour_base_, font_base_, bitmap_base_, scroll_x_, scroll_y_, border_,
                           function_, param_start_, result_, chain_, function_bitmap_base_, mouse_speed_, storage_status_, debug_}) {
        w.u8(v);
    }
    for (unsigned i = 0; i < 32; ++i) {
        w.u8(vsid_.reg(i));
    }
    w.u64(last_start_);
    w.u8(static_cast<std::uint8_t>(overlay_state_));
    for (bool v : {restart_, last_display_, last_vsync_, irq_}) {
        w.boolean(v);
    }
    w.u8(sync_vsyncs_);
    w.u32(paper_px_);
    w.bytes(shown_);
    w.bytes(next_);
    w.u8(held_);
    w.bytes(bg_mask_);
    for (std::uint8_t v : {text_x_, text_y_, text_colour_, pen_colour_}) {
        w.u8(v);
    }
    for (const FatFile& f : fat_files_) {
        w.boolean(f.used);
        w.boolean(f.open);
        w.u8(f.volume);
        w.text(f.path);
        w.u64(f.pos);
        w.u8(f.mode);
    }
    for (const FatDir& d : fat_dirs_) {
        w.boolean(d.used);
        w.boolean(d.open);
        w.u8(d.volume);
        w.text(d.path);
        w.u32(d.index);
    }
    for (const LfsFile& f : lfs_files_) {
        w.boolean(f.used);
        w.boolean(f.open);
        w.u8(f.drive);
        w.text(f.path);
        w.u32(static_cast<std::uint32_t>(f.flags));
        w.u32(f.pos);
    }
    for (const LfsDir& d : lfs_dirs_) {
        w.boolean(d.used);
        w.u8(d.drive);
        w.text(d.path);
        w.u32(d.index);
    }
    for (const std::string* s : {&fat_cwd_[0], &fat_cwd_[1], &psram_cwd_, &flash_cwd_, &temp_path_, &dsk_path_}) {
        w.text(*s);
    }
    w.u8(fat_drive_);
    w.u8(dsk_type_);
    w.boolean(psram_drive_);
    w.u32(psram_blocks_);
    w.u32(free_psram_start_);
    w.u8(static_cast<std::uint8_t>(usb_));
    for (std::uint8_t v : {mouse_buttons_, mouse_dx_, mouse_dy_}) {
        w.u8(v);
    }
    w.u32(static_cast<std::uint32_t>(mouse_dx_sum_));
    w.u32(static_cast<std::uint32_t>(mouse_dy_sum_));
    w.boolean(mouse_dx_read_);
    w.boolean(mouse_dy_read_);
    vsid_.save(w);
    w.u64(mcu_start_);
    w.f32(vsid_level_);
    w.boolean(vsid_drop_);
}

void Card256::load(ChunkReader& r) {
    const std::uint8_t version = r.u8();
    if (version < 1 || version > kSaveVersion) {
        r.fail();  // a layout this version does not know
        return;
    }
    select_rom_bank(r.u8());
    r.packed(std::span<std::uint8_t>(iomem_).subspan(kRomBankSize));
    r.packed(fast_);
    psram_.fill(nullptr);
    const unsigned pages = r.u16();
    for (unsigned n = 0; n < pages && !r.failed(); ++n) {
        const unsigned i = r.u16();
        auto page = std::make_shared<PsramPage>();
        r.packed(*page);
        psram_[i % kPsramPages] = std::move(page);
    }
    for (unsigned set = 0; set < 2; ++set) {
        select_[set] = r.u8();
        increment_[set] = r.u8();
        unit_page_[set] = r.u8();
        psram_mapped_[set] = r.u16();
    }
    for (Sprite& s : sprites_) {
        s.x = r.u16();
        s.y = r.u16();
        s.colour = r.u8();
        s.phase = r.u8();
        s.foreground = r.u8();
        s.enabled = r.u8();
        s.full_colour = r.u8();
    }
    rebuild_sprite_words();
    for (std::uint16_t* v : {&sp_sp_collision_, &sp_bg_collision_, &sp_sp_mask_, &sp_bg_mask_, &line_, &max_line_, &screen_height_}) {
        *v = r.u16();
    }
    for (std::uint8_t* v : {&sprite_base_, &video_mode_, &screen_base_, &colour_base_, &font_base_, &bitmap_base_, &scroll_x_, &scroll_y_,
                            &border_, &function_, &param_start_, &result_, &chain_, &function_bitmap_base_, &mouse_speed_,
                            &storage_status_, &debug_}) {
        *v = r.u8();
    }
    vsid_.power_on();
    mcu_start_ = 0;
    vsid_level_ = 0.0f;
    vsid_drop_ = false;
    for (unsigned i = 0; i < 32; ++i) {
        vsid_.set_reg(i, r.u8());
    }
    last_start_ = r.u64();
    if (version < 2) {
        return;  // a version 1 snapshot: the overlay as after power-on
    }
    overlay_state_ = static_cast<OverlayState>(std::min<std::uint8_t>(r.u8(), 2));
    for (bool* v : {&restart_, &last_display_, &last_vsync_, &irq_}) {
        *v = r.boolean();
    }
    sync_vsyncs_ = r.u8();
    paper_px_ = r.u32();
    r.bytes(shown_);
    r.bytes(next_);
    held_ = r.u8();
    r.bytes(bg_mask_);
    for (std::uint8_t* v : {&text_x_, &text_y_, &text_colour_, &pen_colour_}) {
        *v = r.u8();
    }
    if (version < 3) {
        return;  // a version 2 snapshot: storage and mouse as after power-on
    }
    for (FatFile& f : fat_files_) {
        f.used = r.boolean();
        f.open = r.boolean();
        f.volume = r.u8() & 1;
        f.path = r.text(512);
        f.pos = r.u64();
        f.mode = r.u8();
    }
    for (FatDir& d : fat_dirs_) {
        d.used = r.boolean();
        d.open = r.boolean();
        d.volume = r.u8() & 1;
        d.path = r.text(512);
        d.index = r.u32();
    }
    for (LfsFile& f : lfs_files_) {
        f.used = r.boolean();
        f.open = r.boolean();
        f.drive = r.u8();
        f.path = r.text(512);
        f.flags = static_cast<int>(r.u32());
        f.pos = r.u32();
    }
    for (LfsDir& d : lfs_dirs_) {
        d.used = r.boolean();
        d.drive = r.u8();
        d.path = r.text(512);
        d.index = r.u32();
    }
    for (std::string* s : {&fat_cwd_[0], &fat_cwd_[1], &psram_cwd_, &flash_cwd_, &temp_path_, &dsk_path_}) {
        *s = r.text(512);
    }
    fat_drive_ = r.u8() & 1;
    dsk_type_ = std::min<std::uint8_t>(r.u8(), 3);
    psram_drive_ = r.boolean();
    psram_blocks_ = std::min<std::uint32_t>(r.u32(), kPsramSize / 4096);
    free_psram_start_ = r.u32();
    usb_ = static_cast<Usb>(std::min<std::uint8_t>(r.u8(), 2));
    for (std::uint8_t* v : {&mouse_buttons_, &mouse_dx_, &mouse_dy_}) {
        *v = r.u8();
    }
    mouse_dx_sum_ = static_cast<std::int32_t>(r.u32());
    mouse_dy_sum_ = static_cast<std::int32_t>(r.u32());
    mouse_dx_read_ = r.boolean();
    mouse_dy_read_ = r.boolean();
    if (version < 4) {
        return;  // a version 3 snapshot: the vSID engine as after power-on, with its registers
    }
    vsid_.load(r);
    mcu_start_ = r.u64();
    vsid_level_ = r.f32();
    vsid_drop_ = r.boolean();
}

}  // namespace freetvc
