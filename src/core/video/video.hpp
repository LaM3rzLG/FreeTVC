// SPDX-License-Identifier: GPL-3.0-or-later
// TVC video generator: 6845 addressing, pixel modes, palette, border and blanking (tvc-hardware.md §5).
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <span>

#include "core/video/framebuffer.hpp"
#include "core/video/mc6845.hpp"

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// A card driving /EXTC and EC0-3: HBA-2 sheet 9 puts them through B11 onto the colour bus that the 74LS298 (B9) latches
class VideoOverlay {
public:
    virtual ~VideoOverlay() = default;
    // Every CRTC character: DE (the bus BORDER), VSYNC, the mode, and the 8 framebuffer pixels when the beam draws paper, else nullptr
    virtual void video_character(bool display, bool vsync, std::uint8_t mode, std::uint8_t* px) = 0;
};

class Video {
public:
    // Snapshot fields, framebuffers included (core/snapshot/machine_state.cpp)
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);
    static constexpr std::size_t kBankSize = 0x4000;

    void power_on();

    void set_border(std::uint8_t value);                    // port 00h, I-G-R-B- (b7, b5, b3, b1)
    void set_palette(unsigned index, std::uint8_t value);  // ports 60h-63h, -I-G-R-B (b6, b4, b2, b0)
    void set_mode(std::uint8_t value) { mode_ = value & 3; }  // port 06h b1-0: 00 2-colour, 01 4-colour, 1x 16-colour

    Mc6845& crtc() { return crtc_; }
    const Mc6845& crtc() const { return crtc_; }

    // Steps every character whose CRTC slot starts at or before `ticks` (half-T); returns the number of cursor pulses.
    unsigned advance_to(std::uint64_t ticks, std::span<const std::uint8_t, kBankSize> vram, VideoOverlay* overlay = nullptr);

    // The last complete frame and the number of frames completed since power-on.
    const Framebuffer& frame() const { return front_; }
    std::uint64_t frame_count() const { return frame_count_; }

    // Beam position: framebuffer row (from the first line after VSYNC) and character column (from HSYNC).
    unsigned beam_line() const { return beam_y_; }
    unsigned beam_column() const { return beam_x_; }

    std::uint8_t border() const { return border_; }
    std::uint8_t palette(unsigned index) const { return palette_[index & 3]; }
    std::uint8_t mode() const { return mode_; }

    // VRAM address of a 6845 output: MA b5-0 = column, RA b1-0 = line in the row, MA b11-6 = row.
    static constexpr std::uint16_t vram_address(std::uint16_t ma, std::uint8_t ra) {
        return static_cast<std::uint16_t>((ma & 0x3F) | (ra & 3) << 6 | ((ma >> 6) & 0x3F) << 8);
    }

    // The 8 dots of a VRAM byte in a mode (0 2-colour, 1 4-colour, 2-3 16-colour) with a palette of IGRB indexes.
    static void decode(std::uint8_t byte, std::uint8_t mode, const std::array<std::uint8_t, 4>& palette, std::uint8_t* px);
    const std::array<std::uint8_t, 4>& palettes() const { return palette_; }

private:
    bool step_character(std::span<const std::uint8_t, kBankSize> vram, VideoOverlay* overlay);
    void new_line();

    Mc6845 crtc_;
    std::uint8_t border_ = 0;               // IGRB index
    std::array<std::uint8_t, 4> palette_{};  // IGRB indexes
    std::uint8_t mode_ = 0;

    std::uint64_t next_slot_ = 2;  // tick of the next CRTC slot (DEB phase 2)
    bool vblank_ = false;          // set by VSYNC, cleared when MA9 goes high
    bool last_hsync_ = false;
    bool last_vsync_ = false;
    bool last_ma9_ = false;
    bool frame_pending_ = false;
    unsigned beam_x_ = 0;  // characters since the line started
    unsigned beam_y_ = 0;  // lines since the frame started

    Framebuffer work_;
    Framebuffer front_;
    std::uint64_t frame_count_ = 0;
};

// The VRAM bytes the 6845 shows with these registers: R6 rows of R1 characters from R12/R13, R9 + 1 lines each (RA b1-0).
std::bitset<Video::kBankSize> displayed_vram(const std::array<std::uint8_t, Mc6845::kRegisters>& regs);

}  // namespace freetvc
