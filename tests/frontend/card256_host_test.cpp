// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++'s pendrive as a host folder: names without case, 8.3 aliases, code page 437, the file operations.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "frontend/common/card256_host.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

namespace fs = std::filesystem;

}  // namespace

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "freetvc-pendrive-test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Games", ec);
    std::ofstream(root / "Hello.CAS", std::ios::binary) << "abc";
    std::ofstream(root / "A long file name.cas", std::ios::binary) << "x";
    std::ofstream(root / fs::path(u8"árvíz.cas"), std::ios::binary) << "y";

    freetvc::PendriveFolder v(root);
    freetvc::FatEntry e;
    check(v.stat("/hello.cas", e) == freetvc::kFrOk && e.name == "Hello.CAS" && e.size == 3, "a name in any case");
    check(e.alt == "HELLO.CAS" && e.attr == freetvc::kAmArchive, "its 8.3 name and attribute");
    check(v.stat("/GAMES", e) == freetvc::kFrOk && e.attr == freetvc::kAmDir, "a directory");
    check(v.stat("/nothing", e) == freetvc::kFrNoFile && v.stat("/none/x", e) == freetvc::kFrNoPath, "missing file and path");
    check(v.stat("/A LONG FILE NAME.CAS", e) == freetvc::kFrOk && e.alt == "ALONGF~1.CAS", "a long name's alias");
    check(v.stat(std::string("/\xA0rv\xA1z.cas"), e) == freetvc::kFrOk, "a host name in code page 437 (\xA0 = a acute)");
    check(freetvc::card256_tvc_name("\xC3\xA1") == "\xA0" && freetvc::card256_host_name("\x82") == "\xC3\xA9", "code page 437 both ways");

    std::vector<freetvc::FatEntry> list;
    check(v.list("/", list) == freetvc::kFrOk && list.size() == 4 && list[0].name == "A long file name.cas" && list[1].name == "Games",
          "the root, sorted without case");

    std::vector<std::uint8_t> buf(10);
    std::size_t n = 0;
    check(v.read("/HELLO.cas", 1, buf, n) == freetvc::kFrOk && n == 2 && buf[0] == 'b', "read from a position");
    const std::vector<std::uint8_t> more{'d', 'e'};
    check(v.write("/hello.cas", 5, more, n) == freetvc::kFrOk && n == 2 && fs::file_size(root / "Hello.CAS") == 7, "write past the end");
    check(v.read("/hello.cas", 3, buf, n) == freetvc::kFrOk && n == 4 && buf[0] == 0 && buf[2] == 'd', "the gap reads as zeros");
    check(v.create("/games/New.cas") == freetvc::kFrOk && fs::exists(root / "Games" / "New.cas"), "create in a directory found without case");
    check(v.create("/games") == freetvc::kFrDenied, "create over a directory: denied");
    check(v.make_dir("/games/sub") == freetvc::kFrOk && v.make_dir("/GAMES/SUB") == freetvc::kFrExist, "mkdir, then exists");
    check(v.rename("/games/new.cas", "/Moved.cas") == freetvc::kFrOk && fs::exists(root / "Moved.cas"), "rename across directories");
    check(v.rename("/moved.cas", "/HELLO.CAS") == freetvc::kFrExist, "rename onto another file: exists");
    check(v.rename("/moved.cas", "/MOVED.CAS") == freetvc::kFrOk && fs::exists(root / "MOVED.CAS"), "rename in case only");
    check(v.remove("/games") == freetvc::kFrDenied, "a directory that is not empty stays");
    check(v.remove("/games/sub") == freetvc::kFrOk && v.remove("/moved.cas") == freetvc::kFrOk, "remove a directory and a file");

    freetvc::Card256Host host;
    std::string error;
    check(!host.plug_pendrive((root / "nothing").string(), error), "a missing folder is refused");
    check(host.plug_pendrive(root.string(), error) && host.usb() == freetvc::Card256Usb::Pendrive && host.media().pendrive, "plug the folder");
    host.plug_mouse();
    check(host.usb() == freetvc::Card256Usb::Mouse && !host.media().pendrive, "the mouse takes the only port");
    fs::create_directories(root / "flash" / "games", ec);
    std::ofstream(root / "flash" / "Launcher.CAS", std::ios::binary) << "L";
    std::ofstream(root / "flash" / "games" / "x.cas", std::ios::binary) << "X";
    check(host.load_flash((root / "flash").string(), error) && host.media().flash_valid &&
              host.media().flash->size() == freetvc::kCard256FlashBytes,
          "a folder packs into a flash image: " + error);
    check(!host.load_flash((root / "Hello.CAS").string(), error), "a file that is no littlefs image is refused");

    fs::remove_all(root, ec);
    if (g_failures == 0) {
        std::printf("card256_host_test: all passed\n");
        return 0;
    }
    std::printf("card256_host_test: %d failure(s)\n", g_failures);
    return 1;
}
