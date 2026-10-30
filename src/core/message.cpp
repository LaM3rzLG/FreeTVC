// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/message.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace freetvc {

namespace {

MessageHook g_hook = nullptr;

template <typename T>
std::string one(const char* spec, T value) {
    const int size = std::snprintf(nullptr, 0, spec, value);
    std::string out(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0) {
        std::snprintf(out.data(), out.size() + 1, spec, value);
    }
    return out;
}

}  // namespace

std::size_t conversion_length(const char* p) {
    const char* q = p + 1;
    while (*q && std::strchr("-+ #0", *q)) {
        ++q;
    }
    while (*q >= '0' && *q <= '9') {
        ++q;
    }
    if (*q == '.') {
        ++q;
        while (*q >= '0' && *q <= '9') {
            ++q;
        }
    }
    if ((q[0] == 'h' && q[1] == 'h') || (q[0] == 'l' && q[1] == 'l')) {
        q += 2;
    } else if (*q && std::strchr("hlzjtL", *q)) {
        ++q;
    }
    return static_cast<std::size_t>(q - p) + (*q ? 1 : 0);
}

std::string msg(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::string out = vmsg(format, args);
    va_end(args);
    return out;
}

std::string vmsg(const char* format, std::va_list args) {
    std::string out;
    std::vector<std::string> parts;
    for (const char* p = format; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        if (p[1] == '%') {
            out += '%';
            p += 2;
            continue;
        }
        const char* start = p;
        p += conversion_length(p);
        char length[3] = {};
        for (const char* l = p - 2; l > start && std::strchr("hlzjtL", *l); --l) {
            std::memmove(length + 1, length, 1);
            length[0] = *l;
        }
        const char type = p > start + 1 ? p[-1] : '\0';
        const std::string spec(start, static_cast<std::size_t>(p - start));
        const std::string_view len(length);
        std::string part;
        switch (type) {
        case 'd':
        case 'i':
            part = len == "ll" ? one(spec.c_str(), va_arg(args, long long))
                   : len == "l" ? one(spec.c_str(), va_arg(args, long))
                   : len == "z" ? one(spec.c_str(), va_arg(args, std::ptrdiff_t))
                   : len == "j" ? one(spec.c_str(), va_arg(args, std::intmax_t))
                   : len == "t" ? one(spec.c_str(), va_arg(args, std::ptrdiff_t))
                                : one(spec.c_str(), va_arg(args, int));
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'c':
            part = len == "ll" ? one(spec.c_str(), va_arg(args, unsigned long long))
                   : len == "l" ? one(spec.c_str(), va_arg(args, unsigned long))
                   : len == "z" ? one(spec.c_str(), va_arg(args, std::size_t))
                   : len == "j" ? one(spec.c_str(), va_arg(args, std::uintmax_t))
                   : len == "t" ? one(spec.c_str(), va_arg(args, std::size_t))
                                : one(spec.c_str(), va_arg(args, unsigned));
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
            part = len == "L" ? one(spec.c_str(), va_arg(args, long double)) : one(spec.c_str(), va_arg(args, double));
            break;
        case 's': part = one(spec.c_str(), va_arg(args, const char*)); break;
        case 'p': part = one(spec.c_str(), va_arg(args, void*)); break;
        default: part = spec; break;  // not a conversion msg() knows: kept as written
        }
        out += part;
        parts.push_back(std::move(part));
    }
    if (g_hook) {
        g_hook(out, format, parts);
    }
    return out;
}

void set_message_hook(MessageHook hook) {
    g_hook = hook;
}

}  // namespace freetvc
