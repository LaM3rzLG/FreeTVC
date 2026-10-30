// SPDX-License-Identifier: GPL-3.0-or-later
// The one lookup every UI string goes through (design/ui.md "Language"); the English text is the key and the fallback.
#pragma once

#include <cstdarg>
#include <string>

#include <vector>

#include "core/message.hpp"
#include "frontend/sdl/translation.hpp"

namespace freetvc::sdl {

const char* tr(const char* english);
// The same where one English word needs two translations, as gettext's msgctxt
const char* trc(const char* context, const char* english);
// printf with the translated format; the compiler checks the arguments against the English one, po_check.py the translation
std::string tr_format(const char* english, ...) FREETVC_PRINTF(1, 2);
std::string vformat(const char* format, std::va_list args);

// Catalogs live as long as the program (one per code), so a pointer tr() returned earlier stays valid
void use_catalog(Catalog catalog);
void use_english();
const std::string& current_language();  // "en" or the catalog's code

// The message hook: keeps the last few thousand msg() results with their formats and parts; thread-safe
void record_message(const std::string& english, const char* format, const std::vector<std::string>& args);
// A recorded message rebuilt in the UI language, its parts too; anything else unchanged
std::string translate(const std::string& english);

}  // namespace freetvc::sdl
