// SPDX-License-Identifier: GPL-3.0-or-later
// CAS parsing and writing (core/media/cas.hpp) without a ROM.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/media/cas.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> program = {0x0D, 0x0A, 0x00, 0xDD, 0x20, 0x22, 'H', 'I', 0x22, 0xFF, 0x00};
    std::vector<std::uint8_t> stream(16, 0);
    stream[1] = 0x01;
    stream[2] = static_cast<std::uint8_t>(program.size());
    stream.insert(stream.end(), program.begin(), program.end());

    const std::vector<std::uint8_t> file = freetvc::build_cas(false, false, stream);
    check(file.size() == 128 + 16 + program.size(), "size = header + stream");
    check(file[0] == 0x11 && file[1] == 0 && file[2] == 1 && file[3] == 0 && file[4] == file.size() - 128,
          "type 11h, records = size / 128, byte 04h = size mod 128");

    std::string error;
    auto cas = freetvc::parse_cas(file, error);
    check(cas && !cas->buffered && cas->warnings.empty(), "a clean file parses without warnings");
    check(cas && cas->header().size() == 16 && cas->block().size() == program.size(), "header and block split");
    check(cas && !cas->autorun(), "autorun byte 0");

    std::vector<std::uint8_t> bad = file;
    bad[2] = 0x07;      // wrong record count, as in 33 of the 112 surveyed files
    bad[0x82] = 0x20;   // program header longer than the file
    bad.push_back(0);   // one extra byte
    cas = freetvc::parse_cas(bad, error);
    check(cas && cas->warnings.size() == 2, "record count and length mismatch are warnings, not errors");
    check(cas && cas->block().size() == program.size() + 1, "the file size decides the block length");

    std::vector<std::uint8_t> buffered = freetvc::build_cas(true, true, std::vector<std::uint8_t>{'A', 'B'});
    cas = freetvc::parse_cas(buffered, error);
    check(cas && cas->buffered && cas->protect && cas->stream.size() == 2, "buffered file with protect byte");

    check(!freetvc::parse_cas(std::vector<std::uint8_t>(100, 0x11), error), "shorter than the UPM header");
    std::vector<std::uint8_t> short_unbuffered(130, 0);
    short_unbuffered[0] = 0x11;
    check(!freetvc::parse_cas(short_unbuffered, error), "unbuffered file without a program header");

    check(freetvc::tvc_file_name("Hello.cas") == "HELLO.CAS", "a-z raised");
    check(freetvc::tvc_file_name(std::string("\x91\x98\x99{", 4)) == std::string("\x81\x88\x99{", 4),
          "accented lower case 90h-98h raised, others kept");
    check(freetvc::tvc_file_name("ABCDEFGHIJKLMNOPQRS").size() == 16, "16 characters at most");

    if (failures == 0) {
        std::printf("PASS\n");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
