// SPDX-License-Identifier: GPL-3.0-or-later
// Translation catalogs (design/ui.md "Language"): the .po reader, the language choice, tr() and tr_format(),
// msg() against printf, messages rebuilt in the UI language, and the shipped catalogs, which must read without errors.
// usage: translation_test <data/lang directory>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/message.hpp"
#include "frontend/sdl/text.hpp"
#include "frontend/sdl/translation.hpp"

namespace {

using freetvc::sdl::Catalog;
using freetvc::sdl::LanguageFile;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string g_hook_format;
std::vector<std::string> g_hook_parts;

void spy(const std::string&, const char* format, const std::vector<std::string>& args) {
    g_hook_format = format;
    g_hook_parts = args;
}

std::string entry(const Catalog& c, const std::string& id) {
    const auto it = c.entries.find(id);
    return it == c.entries.end() ? "<none>" : it->second;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: translation_test <data/lang directory>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];

    // The reader: header, escapes, continuation lines, fuzzy, plural and untranslated entries, msgctxt, comments
    std::vector<std::string> errors;
    const Catalog c = freetvc::sdl::parse_po(
        "# a comment\n"
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\"Language: hu\\n\"\n"
        "\"X-Language-Name: Magyar\\n\"\n"
        "\n"
        "#: src/a.cpp:1\n"
        "msgid \"Quit\"\n"
        "msgstr \"Kilépés\"\n"
        "\n"
        "msgid \"Two\\nlines \\\"quoted\\\" \\\\ \\x41\\101\\t\"\n"
        "msgstr \"\"\n"
        "\"Két\\n\"\n"
        "\"sor\"\n"
        "#, fuzzy\n"
        "msgid \"Fuzzy\"\n"
        "msgstr \"Bizonytalan\"\n"
        "\n"
        "#, c-format\n"
        "msgid \"%s file\"\n"
        "msgid_plural \"%s files\"\n"
        "msgstr[0] \"%s fájl\"\n"
        "\n"
        "msgid \"Untranslated\"\n"
        "msgstr \"\"\n"
        "\n"
        "msgctxt \"tape\"\n"
        "msgid \"Stop\"\n"
        "msgstr \"Állj\"\n"
        "#~ msgid \"Old\"\n"
        "#~ msgstr \"Régi\"\n",
        errors);
    check(errors.empty(), "the sample reads without errors" + (errors.empty() ? std::string() : ": " + errors[0]));
    check(c.code == "hu" && c.name == "Magyar", "header: Language and X-Language-Name");
    check(entry(c, "Quit") == "Kilépés", "a plain entry");
    check(entry(c, "Two\nlines \"quoted\" \\ AA\t") == "Két\nsor", "escapes and continuation lines");
    check(c.entries.count("Fuzzy") == 0, "fuzzy entries are left out");
    check(c.entries.count("%s file") == 0, "plural entries are left out");
    check(c.entries.count("Untranslated") == 0, "untranslated entries are left out");
    check(entry(c, std::string("tape") + '\x04' + "Stop") == "Állj", "msgctxt goes in front of the key");
    check(c.entries.count("Old") == 0, "obsolete entries are left out");
    check(c.entries.size() == 3, "three entries");

    errors.clear();
    freetvc::sdl::parse_po("msgid \"a\"\nmsgstr \"b\n\"stray\nmsgfoo \"x\"\n\"orphan\"\n", errors);
    check(errors.size() == 4, "four malformed lines reported, got " + std::to_string(errors.size()));
    check(!errors.empty() && errors[0].starts_with("line 2:"), "errors carry their line number");
    errors.clear();
    freetvc::sdl::parse_po("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=ISO-8859-2\\n\"\n", errors);
    check(errors.size() == 1, "a charset other than UTF-8 is reported");

    // The OS's preferred languages
    const std::vector<LanguageFile> hu{{"hu", "Magyar", {}}};
    check(freetvc::sdl::pick_language({"hu_HU.UTF-8", "en"}, hu) == "hu", "hu_HU picks hu");
    check(freetvc::sdl::pick_language({"de", "HU"}, hu) == "hu", "a language without a catalog is skipped");
    check(freetvc::sdl::pick_language({"en_GB", "hu"}, hu) == "en", "English first stays English");
    check(freetvc::sdl::pick_language({"de"}, hu) == "en", "no match falls back to English");
    check(freetvc::sdl::pick_language({}, hu) == "en", "no preference is English");

    // tr() and tr_format(): English without a catalog and for missing keys; earlier results stay valid after a switch
    using freetvc::sdl::tr;
    check(std::string(tr("Quit")) == "Quit", "English before any catalog");
    freetvc::sdl::use_catalog(c);
    check(freetvc::sdl::current_language() == "hu", "the catalog's code is current");
    const char* quit = tr("Quit");
    check(std::string(quit) == "Kilépés", "tr() translates");
    check(std::string(tr("Missing")) == "Missing", "a missing key falls back to English");
    Catalog other;
    other.code = "hu";
    other.entries["Quit"] = "x";
    freetvc::sdl::use_catalog(other);
    check(std::string(tr("Quit")) == "Kilépés", "a second catalog with the same code keeps the first");
    Catalog numbers;
    numbers.code = "xx";
    numbers.entries["%d of %s"] = "%d / %s";
    freetvc::sdl::use_catalog(numbers);
    check(freetvc::sdl::tr_format("%d of %s", 3, "five") == "3 / five", "tr_format() formats the translation");
    check(std::string(quit) == "Kilépés", "an earlier result survives a switch");
    freetvc::sdl::use_english();
    check(freetvc::sdl::current_language() == "en" && std::string(tr("Quit")) == "Quit", "back to English");
    check(freetvc::sdl::tr_format("%d of %s", 3, "five") == "3 of five", "tr_format() in English");

    // msg(): the same text as printf, and the hook sees the format and each conversion's text
    {
        char expected[160];
        std::snprintf(expected, sizeof expected, "%d|%u|%zu|%llu|%02X|%c|%s|%.2f|%5.1f%%|%-4s|%08X", -7, 7u, std::size_t{12}, 1ULL << 40, 0xAu, 'q',
                      "text", 3.14159, 2.5, "ab", 0xDEADBEEFu);
        freetvc::set_message_hook(&spy);
        const std::string got = freetvc::msg("%d|%u|%zu|%llu|%02X|%c|%s|%.2f|%5.1f%%|%-4s|%08X", -7, 7u, std::size_t{12}, 1ULL << 40, 0xAu, 'q',
                                             "text", 3.14159, 2.5, "ab", 0xDEADBEEFu);
        check(got == expected, "msg() matches printf: " + got + " / " + expected);
        check(g_hook_format == "%d|%u|%zu|%llu|%02X|%c|%s|%.2f|%5.1f%%|%-4s|%08X", "the hook gets the format");
        const std::vector<std::string> parts{"-7", "7", "12", "1099511627776", "0A", "q", "text", "3.14", "  2.5", "ab  ", "DEADBEEF"};
        check(g_hook_parts == parts, "the hook gets each conversion's text");
        check(freetvc::msg("no conversions") == "no conversions" && g_hook_parts.empty(), "a plain message has no parts");
        check(freetvc::conversion_length("%-08.3llx tail") == 9 && freetvc::conversion_length("%s") == 2, "conversion_length()");
    }

    // translate(): recorded messages rebuilt in the catalog's language, their parts too; others unchanged
    {
        freetvc::set_message_hook(&freetvc::sdl::record_message);
        Catalog hu8b;
        hu8b.code = "zz";
        hu8b.entries["cannot write %s"] = "nem írható: %s";
        hu8b.entries["%s: %s"] = "%s -> %s";
        hu8b.entries["no tape"] = "nincs szalag";
        hu8b.entries["slot %u holds the %s"] = "%u. hely: %s";
        hu8b.entries["broken %s"] = "%s %s";
        freetvc::sdl::use_catalog(hu8b);
        const std::string inner = freetvc::msg("cannot write %s", "x.cas");
        const std::string outer = freetvc::msg("%s: %s", "file", inner.c_str());
        check(outer == "file: cannot write x.cas", "msg() stays English");
        check(freetvc::sdl::translate(outer) == "file -> nem írható: x.cas", "nested messages are rebuilt: " + freetvc::sdl::translate(outer));
        check(freetvc::sdl::translate(freetvc::msg("no tape")) == "nincs szalag", "a message without parts");
        check(freetvc::sdl::translate(freetvc::msg("slot %u holds the %s", 2u, "GameCard")) == "2. hely: GameCard", "numbers keep their text");
        check(freetvc::sdl::translate("never recorded") == "never recorded", "an unknown message stays as it is");
        const std::string broken = freetvc::msg("broken %s", "a");
        check(freetvc::sdl::translate(broken) == broken, "a translation with more conversions falls back to English");
        const std::string first = freetvc::msg("cannot write %s", "first");
        for (unsigned i = 0; i < 5000; ++i) {
            freetvc::msg("%u", i);
        }
        check(freetvc::sdl::translate(first) == first, "the oldest messages are dropped");
        check(freetvc::sdl::translate(freetvc::msg("cannot write %s", "again")) == "nem írható: again", "new ones still translate");
        freetvc::sdl::use_english();
        check(freetvc::sdl::translate(outer) == outer, "English rebuilds English");
        freetvc::set_message_hook(nullptr);
    }

    // The shipped catalogs
    const std::vector<LanguageFile> shipped = freetvc::sdl::list_languages(dir);
    check(!shipped.empty(), "data/lang has a catalog");
    for (const LanguageFile& file : shipped) {
        std::ifstream in(file.path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        errors.clear();
        const Catalog catalog = freetvc::sdl::parse_po(text, errors);
        check(errors.empty(), file.path.string() + " reads without errors" + (errors.empty() ? std::string() : ": " + errors[0]));
        check(file.name != file.code, file.path.string() + " has X-Language-Name");
        check(catalog.entries.size() > 400, file.path.string() + " has " + std::to_string(catalog.entries.size()) + " entries");
        std::printf("%s: %s (%s), %zu entries\n", file.path.filename().string().c_str(), file.name.c_str(), file.code.c_str(),
                    catalog.entries.size());
    }

    std::printf("%s\n", g_failures == 0 ? "translation_test: all passed" : "translation_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
