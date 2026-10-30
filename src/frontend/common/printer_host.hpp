// SPDX-License-Identifier: GPL-3.0-or-later
// The printer behind the TVC's Centronics port (design/machine.md "Printer port"): the raw bytes to a file, the
// same as UTF-8 text to another, and the last 64 KiB kept for the monitor and MCP.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/machine/printer_port.hpp"

namespace freetvc {

// TVC character codes to UTF-8 text: CR or CR LF ends a line, accented letters 80h-98h become theirs; other control
// codes, and ESC with the byte after it, are left out. `state` carries a CR or ESC across calls.
std::string printer_text(std::string_view bytes, unsigned& state);

class PrinterHost final : public PrinterPort {
public:
    static constexpr std::size_t kKept = 64 * 1024;

    ~PrinterHost() override;
    // Either path may be empty; the files are appended to, like paper fed on. False with `error` set.
    bool plug(const std::string& raw, const std::string& text, std::string& error);
    void unplug();
    bool plugged() const { return plugged_; }
    // What was printed since the last call, as raw bytes
    std::string take();
    std::uint64_t printed() const { return count_; }
    std::string describe() const;
    // Writes buffered output to the files
    void flush();

    void printer_byte(std::uint64_t tick, std::uint8_t value) override;

private:
    bool plugged_ = false;
    std::filesystem::path raw_path_, text_path_;
    std::ofstream raw_, text_;
    unsigned text_state_ = 0;
    std::string kept_;
    std::uint64_t count_ = 0;
};

}  // namespace freetvc
