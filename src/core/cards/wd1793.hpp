// SPDX-License-Identifier: GPL-3.0-or-later
// The WD1793 / FD1793 floppy controller at 1 MHz with 250 kbit/s MFM, as on the HBF card (design/cards.md "WD1793").
// Time comes from the machine's half-T ticks; the controller catches up whenever the CPU reaches it.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace freetvc {

class ChunkWriter;
class ChunkReader;
class FloppyDrives;

class Wd1793 {
public:
    static constexpr std::uint64_t kTicksPerMs = 6250;
    static constexpr std::uint64_t kByteTicks = 200;  // 32 µs a byte
    static constexpr unsigned kTrackBytes = 6250;     // one revolution at 300 rpm
    static constexpr std::uint64_t kRevolutionTicks = kTrackBytes * kByteTicks;
    static constexpr unsigned kCylinders = 84;  // the drive mechanics' last cylinder + 1

    // Master reset: registers cleared, nothing running.
    void reset();

    // Registers 0-3: status / command, track, sector, data.
    std::uint8_t read(unsigned reg, std::uint64_t tick, FloppyDrives* drives);
    void write(unsigned reg, std::uint8_t value, std::uint64_t tick, FloppyDrives* drives);
    // The DRQ and INTRQ outputs.
    bool drq(std::uint64_t tick, FloppyDrives* drives);
    bool intrq(std::uint64_t tick, FloppyDrives* drives);
    // The card's drive latch: selected drive (-1 = none), side select and the motor line.
    void select(int drive, unsigned side, bool motor, std::uint64_t tick, FloppyDrives* drives);

    // For the front ends; no side effects.
    bool busy() const { return busy_; }
    int selected_drive() const { return drive_; }
    bool motor() const { return motor_; }
    unsigned cylinder(unsigned drive) const { return drive < cylinders_.size() ? cylinders_[drive] : 0; }
    std::uint8_t track_register() const { return track_; }
    std::uint8_t sector_register() const { return sector_; }
    std::uint8_t command() const { return command_; }

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    enum class Phase : std::uint8_t {
        None, Step, Settle, SearchId, ReadData, WriteFirst, WriteData, ReadAddress, WaitIndex, ReadTrack, WriteTrack, Finish
    };

    void advance(std::uint64_t tick, FloppyDrives* drives);
    void run_event(std::uint64_t t, FloppyDrives* drives);
    void start(std::uint8_t command, std::uint64_t tick, FloppyDrives* drives);
    void finish(std::uint64_t t, std::uint8_t error_bits);
    void step_once();
    // Marks the selected drive active at `t`
    void touch(std::uint64_t t, FloppyDrives* drives) const;
    void search(std::uint64_t t, FloppyDrives* drives);
    void write_track_data(FloppyDrives* drives);
    std::uint8_t status(std::uint64_t tick, FloppyDrives* drives);

    bool ready(FloppyDrives* drives) const;
    bool spinning(FloppyDrives* drives) const;
    bool index_hole(std::uint64_t tick, FloppyDrives* drives) const;
    bool write_protected(FloppyDrives* drives) const;
    // Sectors on the selected drive's current cylinder and side (0 when there is no such track)
    unsigned track_sectors(FloppyDrives* drives) const;
    // Byte offset of sector index i's ID address mark from the index hole
    unsigned id_mark(unsigned i, unsigned sectors) const;
    std::uint64_t next_pass(std::uint64_t t, unsigned byte) const;
    std::uint64_t fifth_index(std::uint64_t t) const;
    void build_track(FloppyDrives* drives);

    // Registers and outputs
    std::uint8_t command_ = 0;
    std::uint8_t track_ = 0;
    std::uint8_t sector_ = 1;
    std::uint8_t data_ = 0;
    std::uint8_t errors_ = 0;  // status bits 2-6 of the last command
    bool type1_ = true;        // status in the type I layout
    bool busy_ = false;
    bool drq_ = false;
    bool intrq_ = false;
    bool intrq_held_ = false;  // force interrupt with I3: only another command clears it
    std::uint64_t index_interrupt_ = 0;  // force interrupt with I2: INTRQ at this tick (0 = none)
    bool head_loaded_ = false;
    std::uint64_t head_used_ = 0;
    std::int8_t direction_ = 1;

    // Drive latch and mechanics
    int drive_ = -1;
    unsigned side_ = 0;
    bool motor_ = false;
    std::array<std::uint8_t, 4> cylinders_{};

    // The running command
    Phase phase_ = Phase::None;
    std::uint64_t next_ = 0;
    std::uint64_t deadline_ = 0;
    unsigned steps_ = 0;
    unsigned count_ = 0;
    unsigned found_ = 0;        // sector index the search found
    std::uint8_t pending_ = 0;  // error bits the Finish event reports
    std::vector<std::uint8_t> buffer_;
};

}  // namespace freetvc
