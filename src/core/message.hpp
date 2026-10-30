// SPDX-License-Identifier: GPL-3.0-or-later
// Messages for people: English text built from a printf format, whose parts a front end may watch to translate it.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <string>
#include <vector>

#if defined(__MINGW32__)
#define FREETVC_PRINTF(format_index, first_arg) __attribute__((format(gnu_printf, format_index, first_arg)))  // MinGW's ANSI stdio
#elif defined(__GNUC__)
#define FREETVC_PRINTF(format_index, first_arg) __attribute__((format(printf, format_index, first_arg)))
#else
#define FREETVC_PRINTF(format_index, first_arg)
#endif

// Marks a literal for tools/lang/po_check.py where it reaches msg() or tr() later
#define N_(english) english

namespace freetvc {

// printf into a string (no '*' widths); the hook, if any, gets the result, the format and each conversion's text
std::string msg(const char* format, ...) FREETVC_PRINTF(1, 2);
std::string vmsg(const char* format, std::va_list args);

// Length of the printf conversion at `p` ('%' included); msg() and the front ends' translation share it
std::size_t conversion_length(const char* p);

using MessageHook = void (*)(const std::string& english, const char* format, const std::vector<std::string>& args);
// Set before other threads start; msg() may be called from any thread, so the hook must be thread-safe
void set_message_hook(MessageHook hook);

}  // namespace freetvc
