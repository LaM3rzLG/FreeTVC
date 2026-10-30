// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++'s vSID (design/cards.md "vSID"): the firmware's SID approximation ([FW] tvc-sid.c, Sanyi on
// Bakisha's STM32-SID-Player) and its sample pipeline ([FW] tvc-sound.c): 985 337 Hz, a sample every 24 clocks, register
// writes queued with their clock, samples computed 128 at a time and played one block later. No filter.
#pragma once

#include <array>
#include <cstdint>
#include <deque>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

class VSid {
public:
    static constexpr unsigned kClocksPerSample = 24;
    static constexpr unsigned kBlockSamples = 128;  // half of the firmware's DMA ring
    static constexpr std::uint64_t kBlockClocks = kClocksPerSample * kBlockSamples;

    // The MCU's start: reset_SID() over the globals' initial values
    void power_on();
    // A write from the TVC at vSID clock `clock`, applied when its block is computed ([FW] sid_write_queue: 256 entries)
    void queue_write(std::uint8_t reg, std::uint8_t value, std::uint64_t clock);
    // Computes every block that ends at or before `clock` ([FW] calculate_sound_samples, one call per block)
    void compute_to(std::uint64_t clock);
    // [FW] SID[]: what the data port reads; 27 and 28 follow voice 3 at every sample
    std::uint8_t reg(unsigned i) const { return sid_[i & 31]; }
    void set_reg(unsigned i, std::uint8_t value) { sid_[i & 31] = value; }

    // Computed samples waiting to play: the 16-bit PWM value ([FW] main_volume) and the clock it starts at
    bool has_output() const { return !output_.empty(); }
    std::uint64_t output_clock() const { return output_clock_; }
    std::uint16_t output_value() const { return output_.front(); }
    void pop_output() {
        output_.pop_front();
        output_clock_ += kClocksPerSample;
    }

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

    static const std::array<std::uint32_t, 16> kAdsrRate;        // [FW] ADSR_LFSR15
    static const std::array<std::uint8_t, 256> kVolumeDivider;   // [FW] ADSR_Volume2LFSR5
    static const std::array<std::uint8_t, 4096> kAndMask;        // [FW] AND_mask (reSID's combined waveforms)

private:
    struct Voice {
        std::uint32_t osc = 0;        // 24-bit accumulator
        std::uint32_t freq = 0;
        std::uint32_t noise_acc = 0;  // counts bit-19 rises for the noise register
        std::uint32_t noise = 0x7FFFF8;
        std::uint32_t pulse_prev = 0;  // the accumulator's top 12 bits at the last calculation
        std::uint16_t pulse_width = 2048;
        std::uint16_t da = 0;  // 12-bit waveform output
        std::uint8_t control = 0;  // noise, pulse, saw, triangle, test, ring, sync, gate (register 4/11/18)
        std::uint8_t msb = 0;
        std::uint8_t msb_prev = 0;
        std::uint8_t rising = 0;
        std::uint8_t gate_prev = 1;
        std::uint8_t attack = 0;
        std::uint8_t decay = 0;
        std::uint8_t sustain = 0;
        std::uint8_t release = 0;
        std::uint8_t stage = 0;  // 0 done, 1 attack, 2 decay, 3 sustain, 4 release
        bool hold_zero = false;
        std::uint32_t envelope = 0;  // 0-255
        std::uint32_t lfsr15 = 0x7FFF;
        std::uint32_t lfsr15_cmp = 0;
        std::uint32_t lfsr5 = 0;
        std::uint32_t lfsr5_cmp = 1;
        std::int32_t volume = 0;  // the voice's signed output
    };

    void set_register(unsigned addr, std::uint8_t value);
    void calculate(std::uint32_t clocks);
    void envelope(Voice& v, std::uint32_t clocks);
    std::uint16_t output();

    std::array<Voice, 3> voice_{};
    std::array<std::uint8_t, 32> sid_{};
    std::uint8_t master_ = 0;
    struct Write {
        std::uint64_t clock = 0;
        std::uint8_t reg = 0;
        std::uint8_t value = 0;
    };
    std::array<Write, 256> queue_{};
    std::uint8_t read_ = 0;
    std::uint8_t write_ = 0;
    std::uint64_t block_ = 0;        // [FW] timerCycle: the next block to compute
    std::uint64_t prev_sample_ = 0;  // [FW] prevSampleTime
    std::uint16_t remaining_ = 0;    // [FW] remainingClocks
    std::deque<std::uint16_t> output_;
    std::uint64_t output_clock_ = kBlockClocks;
};

}  // namespace freetvc
