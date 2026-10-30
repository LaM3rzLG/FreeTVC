// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/printer_host.hpp"

#include <array>

#include "core/input/keyboard.hpp"
#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr unsigned kAfterCr = 1, kAfterEsc = 2;

void append_utf8(std::string& out, char32_t ch) {
    if (ch < 0x80) {
        out += static_cast<char>(ch);
    } else if (ch < 0x800) {
        out += static_cast<char>(0xC0 | (ch >> 6));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (ch >> 12));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    }
}

const std::array<char32_t, 256>& high_codes() {
    static const std::array<char32_t, 256> table = [] {
        std::array<char32_t, 256> t{};
        for (const CharKey& key : char_keys()) {
            if (key.code >= 0x80) {
                t[key.code] = key.ch;
            }
        }
        return t;
    }();
    return table;
}

}  // namespace

std::string printer_text(std::string_view bytes, unsigned& state) {
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<std::uint8_t>(c);
        const unsigned before = state;
        state = 0;
        if (before == kAfterEsc) {
            continue;
        }
        if (b == 0x0D) {
            out += '\n';
            state = kAfterCr;
        } else if (b == 0x0A) {
            if (before != kAfterCr) {
                out += '\n';
            }
        } else if (b == 0x1B) {
            state = kAfterEsc;
        } else if (b == 0x09 || b == 0x0C) {
            out += static_cast<char>(b);
        } else if (b >= 0x20 && b < 0x7F) {
            out += static_cast<char>(b);
        } else if (b >= 0x80) {
            append_utf8(out, high_codes()[b] ? high_codes()[b] : U'�');
        }
    }
    return out;
}

PrinterHost::~PrinterHost() { flush(); }

bool PrinterHost::plug(const std::string& raw, const std::string& text, std::string& error) {
    std::ofstream raw_file, text_file;
    if (!raw.empty()) {
        raw_file.open(raw, std::ios::binary | std::ios::app);
        if (!raw_file) {
            error = msg("cannot write '%s'", raw.c_str());
            return false;
        }
    }
    if (!text.empty()) {
        text_file.open(text, std::ios::binary | std::ios::app);
        if (!text_file) {
            error = msg("cannot write '%s'", text.c_str());
            return false;
        }
    }
    unplug();
    raw_ = std::move(raw_file);
    text_ = std::move(text_file);
    raw_path_ = raw;
    text_path_ = text;
    text_state_ = 0;
    plugged_ = true;
    return true;
}

void PrinterHost::unplug() {
    flush();
    raw_.close();
    text_.close();
    raw_path_.clear();
    text_path_.clear();
    plugged_ = false;
}

std::string PrinterHost::take() {
    std::string out;
    out.swap(kept_);
    return out;
}

void PrinterHost::flush() {
    if (raw_.is_open()) {
        raw_.flush();
    }
    if (text_.is_open()) {
        text_.flush();
    }
}

void PrinterHost::printer_byte(std::uint64_t, std::uint8_t value) {
    ++count_;
    const char c = static_cast<char>(value);
    if (kept_.size() >= kKept) {
        kept_.erase(0, kept_.size() - kKept + 1);
    }
    kept_ += c;
    if (raw_.is_open()) {
        raw_.put(c);
    }
    if (text_.is_open()) {
        text_ << printer_text(std::string_view(&c, 1), text_state_);
    }
}

std::string PrinterHost::describe() const {
    if (!plugged_) {
        return "no printer (an LPRINT waits after its first character until STOP)";
    }
    std::string text = "printer attached, " + std::to_string(count_) + " bytes printed";
    if (!raw_path_.empty()) {
        text += ", raw to " + raw_path_.string();
    }
    if (!text_path_.empty()) {
        text += ", text to " + text_path_.string();
    }
    if (raw_path_.empty() && text_path_.empty()) {
        text += ", kept in memory only";
    }
    return text;
}

}  // namespace freetvc
