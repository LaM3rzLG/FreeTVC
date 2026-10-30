// SPDX-License-Identifier: GPL-3.0-or-later
// Tape signals and the deck (core/media/tape_signal.hpp, tape_deck.hpp) without a ROM.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/media/tape_deck.hpp"
#include "core/media/tape_signal.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

freetvc::TapeProgram program(std::size_t size) {
    freetvc::TapeProgram p;
    p.name = "DEMO";
    p.header.assign(16, 0);
    p.header[1] = 0x01;
    p.header[2] = static_cast<std::uint8_t>(size);
    p.header[3] = static_cast<std::uint8_t>(size >> 8);
    for (std::size_t i = 0; i < size; ++i) {
        p.data.push_back(static_cast<std::uint8_t>(i * 7 + 3));
    }
    return p;
}

// What cas2ttp.pas writes (tvc.homeserver.hu's CAS/TTP/WAV tools): 257 bytes read per full sector
std::vector<std::uint8_t> cas2ttp_like(const freetvc::TapeProgram& p) {
    std::vector<std::uint8_t> out = {'T', 'T', 'P', '0'};
    const auto add_crc = [&](std::size_t from) {
        const std::uint16_t crc = freetvc::tape_crc(0, std::span<const std::uint8_t>(out).subspan(from));
        out.push_back(static_cast<std::uint8_t>(crc));
        out.push_back(static_cast<std::uint8_t>(crc >> 8));
    };
    std::size_t from = out.size() + 1;
    out.insert(out.end(), {0x00, 0x6A, 0xFF, 0x11, 0x00, 0x01, 0x00, static_cast<std::uint8_t>(1 + p.name.size() + 16),
                           static_cast<std::uint8_t>(p.name.size())});
    out.insert(out.end(), p.name.begin(), p.name.end());
    out.insert(out.end(), p.header.begin(), p.header.end());
    out.push_back(0x00);
    add_crc(from);
    const std::size_t full = p.data.size() / 256;
    from = out.size() + 1;
    out.insert(out.end(), {0x00, 0x6A, 0x00, 0x11, 0x00, static_cast<std::uint8_t>(full + 1)});
    std::size_t next = 0;
    const auto get = [&] { return next < p.data.size() ? p.data[next++] : (++next, std::uint8_t{0xEE}); };
    for (std::size_t s = 1; s <= full + 1; ++s) {
        if (s > 1) {
            from = out.size();
        }
        const bool last = s == full + 1;
        out.push_back(static_cast<std::uint8_t>(s));
        out.push_back(last ? static_cast<std::uint8_t>(p.data.size() % 256) : 0);
        const std::size_t count = last ? p.data.size() % 256 : 257;
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(get());
        }
        out.push_back(last ? 0xFF : 0x00);
        add_crc(from);
    }
    return out;
}

}  // namespace

int main() {
    using namespace freetvc;

    // Blocks: header with name and program header, data in 256-byte sectors, the last one flagged
    const TapeProgram demo = program(600);
    const auto blocks = tape_blocks(demo);
    check(blocks.size() == 2, "header block and one data block");
    check(blocks[0][0] == 0x00 && blocks[0][1] == 0x6A && blocks[0][2] == 0xFF && blocks[0][3] == 0x11 && blocks[0][5] == 1,
          "header block start");
    check(blocks[1][2] == 0x00 && blocks[1][5] == 3, "data block: 3 sectors for 600 bytes");
    check(blocks[1].size() == 6 + 3 * 5 + 600, "data block size: sector number, count, EOF and CRC per sector");
    check(blocks[1][6 + 2 + 256] == 0x00 && blocks[1].back() != 0 && blocks[1][blocks[1].size() - 3] == 0xFF,
          "EOF flag 00h, FFh in the last sector");

    // Encoding and decoding find the same bytes again
    const std::vector<TapeProgram> one{demo};
    const TapeSignal signal = encode_tape(one);
    check(signal.program_ends.size() == 1 && signal.program_autorun.size() == 1 && signal.name == "DEMO", "program bookkeeping");
    const double seconds = static_cast<double>(signal.length()) / kTapeTicksPerSecond;
    check(seconds > 13.0 && seconds < 14.5, "about 13.7 s: 4 s of silence, 15 370 leader cycles (7.2 s), 5 200 bits (2.5 s)");
    check(decode_tape(signal) == blocks, "decode(encode) gives the blocks back");

    // TTP: our own blocks, and cas2ttp's 257-byte sectors
    std::vector<std::uint8_t> ttp = {'T', 'T', 'P', '0'};
    for (const auto& b : blocks) {
        ttp.insert(ttp.end(), b.begin(), b.end());
    }
    std::string error;
    auto parsed = parse_ttp(ttp, error);
    check(parsed && parsed->size() == 1 && (*parsed)[0].name == "DEMO" && (*parsed)[0].data == demo.data &&
              (*parsed)[0].header == demo.header,
          "a clean TTP parses back to the program");
    parsed = parse_ttp(cas2ttp_like(demo), error);
    check(parsed && parsed->size() == 1 && (*parsed)[0].data == demo.data, "cas2ttp's 257-byte sectors give the program back");
    check(!parse_ttp(std::vector<std::uint8_t>{'T', 'T', 'P', '1'}, error), "a wrong magic is refused");
    ttp[20] ^= 0x01;
    check(!parse_ttp(ttp, error) && error.find("CRC") != std::string::npos, "a damaged byte fails the CRC");

    // WAV out and back in, at two rates
    for (const unsigned rate : {44100u, 22050u}) {
        const auto wav = signal_wav(signal, rate);
        auto back = wav_signal(wav, error);
        check(back.has_value(), "the written WAV reads back");
        check(back && decode_tape(*back) == blocks, rate == 44100 ? "decode(WAV 44.1 kHz)" : "decode(WAV 22.05 kHz)");
    }

    // A 441 us square wave sampled at 44.1 kHz (19.45 samples a cycle), each sample the average of its interval as an ADC
    // gives it: the edges come back between samples, so the cycles do not jump between 19 and 20 samples (Space Sapper)
    {
        constexpr unsigned kRate = 44100;
        constexpr double kPeriod = 441e-6;
        std::vector<std::uint8_t> pcm;
        for (unsigned k = 0; k < kRate / 4; ++k) {
            double high = 0.0;  // part of the sample interval spent high
            constexpr int kSteps = 64;
            for (int j = 0; j < kSteps; ++j) {
                const double t = (k + (j + 0.5) / kSteps) / kRate;
                high += std::fmod(t, kPeriod) < kPeriod / 2 ? 1.0 / kSteps : 0.0;
            }
            pcm.push_back(static_cast<std::uint8_t>(std::lround(high * 255.0)));
        }
        std::vector<std::uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                                         0x44, 0xAC, 0, 0, 0x44, 0xAC, 0, 0, 1, 0, 8, 0, 'd', 'a', 't', 'a'};
        for (int i = 0; i < 4; ++i) {
            wav.push_back(static_cast<std::uint8_t>(pcm.size() >> (8 * i)));
        }
        wav.insert(wav.end(), pcm.begin(), pcm.end());
        const auto square = wav_signal(wav, error);
        double worst = 0.0;
        if (square) {
            // Full cycles from one rising edge to the next, skipping the first and the cut-off last run
            for (std::size_t r = 1; r + 2 < square->runs.size(); r += 2) {
                const double us = static_cast<double>(square->runs[r] + square->runs[r + 1]) * 1e6 / kTapeTicksPerSecond;
                worst = std::max(worst, std::abs(us - kPeriod * 1e6));
            }
        }
        check(square && square->runs.size() > 1000, "the square wave reads back");
        check(worst < 4.0, "sub-sample edges: every cycle within 4 us of 441 us (19 or 20 whole samples are 10-12 us off)");
    }

    // The deck: the tape moves only while PLAY is down and the motor is on
    TapeDeck deck;
    deck.insert(signal);
    deck.tape_motor(0, true);
    check(!deck.tape_input(1000) && deck.position() == 0, "stopped: no movement");
    deck.play();
    deck.tape_input(1000);
    check(deck.position() == 0, "PLAY counts from the next tick on");
    deck.tape_input(5000);
    check(deck.position() == 4000, "moving with the motor on");
    deck.tape_motor(6000, false);
    deck.tape_input(9000);
    check(deck.position() == 5000, "the motor stops the tape");
    // Silence for 2 s, then the leader's high half
    deck.tape_motor(9000, true);
    deck.tape_input(9000 + 2 * kTapeTicksPerSecond - 5000 + 10);
    check(deck.tape_input(9000 + 2 * kTapeTicksPerSecond - 5000 + 10), "the first leader cycle starts high after 2 s");
    deck.tape_input(10'000'000'000ULL);
    check(deck.position() == deck.length() && !deck.tape_input(10'000'000'001ULL), "the end of the tape reads low");

    // Recording: the output edges at the tape position become the tape
    TapeDeck recorder;
    recorder.insert_blank();
    recorder.record();
    recorder.tape_motor(100, true);
    std::uint64_t t = 100;
    bool level = false;
    for (int i = 0; i < 6; ++i) {
        t += 1000 + static_cast<std::uint64_t>(i) * 10;
        level = !level;
        recorder.tape_output(t, level);
    }
    recorder.tape_motor(t + 500, false);
    recorder.stop();
    const auto& rec = recorder.signal();
    check(rec.runs.size() == 7 && rec.runs[0] == 1000 && rec.runs[5] == 1050 && rec.runs[6] == 500, "recorded runs");
    check(recorder.length() == 6650 && !rec.first_level, "recorded length and first level");

    std::printf(failures ? "tape_test: %d failure(s)\n" : "tape_test: all checks passed\n", failures);
    return failures ? 1 : 0;
}
