// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/text.hpp"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace freetvc::sdl {

namespace {

std::deque<Catalog>& catalogs() {
    static std::deque<Catalog> all;
    return all;
}

const Catalog* active = nullptr;
const std::string english_code = "en";

constexpr std::size_t kRecordedMessages = 4096;
constexpr int kMessageDepth = 8;

struct Recorded {
    const char* format;
    std::vector<std::string> parts;
};

struct MessageLog {
    std::mutex mutex;
    std::unordered_map<std::string, Recorded> by_text;
    std::deque<std::string> order;  // oldest first, for dropping
};

MessageLog& message_log() {
    static MessageLog log;
    return log;
}

// Lock held by the caller
std::string rebuild(const MessageLog& log, const std::string& english, int depth) {
    const auto it = log.by_text.find(english);
    if (it == log.by_text.end() || depth > kMessageDepth) {
        return english;
    }
    const char* format = tr(it->second.format);
    if (format == it->second.format && it->second.parts.empty()) {
        return english;
    }
    std::string out;
    std::size_t next = 0;
    for (const char* p = format; *p;) {
        if (*p != '%') {
            out += *p++;
        } else if (p[1] == '%') {
            out += '%';
            p += 2;
        } else {
            if (next == it->second.parts.size()) {
                return english;  // a catalog entry with more conversions than the source
            }
            out += rebuild(log, it->second.parts[next++], depth + 1);
            p += conversion_length(p);
        }
    }
    return out;
}

}  // namespace

const char* tr(const char* english) {
    if (!active) {
        return english;
    }
    const auto it = active->entries.find(std::string_view(english));
    return it == active->entries.end() ? english : it->second.c_str();
}

const char* trc(const char* context, const char* english) {
    if (!active) {
        return english;
    }
    const auto it = active->entries.find(std::string(context) + '\x04' + english);
    return it == active->entries.end() ? english : it->second.c_str();
}

std::string tr_format(const char* english, ...) {
    std::va_list args;
    va_start(args, english);
    std::string out = vformat(tr(english), args);
    va_end(args);
    return out;
}

std::string vformat(const char* format, std::va_list args) {
    std::va_list again;
    va_copy(again, args);
    const int size = std::vsnprintf(nullptr, 0, format, args);
    std::string out(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0) {
        std::vsnprintf(out.data(), out.size() + 1, format, again);
    }
    va_end(again);
    return out;
}

void record_message(const std::string& english, const char* format, const std::vector<std::string>& args) {
    MessageLog& log = message_log();
    const std::lock_guard lock(log.mutex);
    if (log.by_text.insert_or_assign(english, Recorded{format, args}).second) {
        log.order.push_back(english);
    }
    while (log.order.size() > kRecordedMessages) {
        log.by_text.erase(log.order.front());
        log.order.pop_front();
    }
}

std::string translate(const std::string& english) {
    MessageLog& log = message_log();
    const std::lock_guard lock(log.mutex);
    return rebuild(log, english, 0);
}

void use_catalog(Catalog catalog) {
    for (const Catalog& loaded : catalogs()) {
        if (loaded.code == catalog.code) {
            active = &loaded;
            return;
        }
    }
    active = &catalogs().emplace_back(std::move(catalog));
}

void use_english() {
    active = nullptr;
}

const std::string& current_language() {
    return active ? active->code : english_code;
}

}  // namespace freetvc::sdl
