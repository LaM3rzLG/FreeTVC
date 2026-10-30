// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/rom_fetch.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "core/message.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/common/rom_list_text.hpp"
#include "frontend/common/unzip.hpp"

namespace fs = std::filesystem;

namespace freetvc {

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    return first == std::string_view::npos ? std::string_view{} : s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

// The next space-separated word of `rest`, which loses it
std::string word(std::string_view& rest) {
    rest = trim(rest);
    const auto space = rest.find(' ');
    std::string w(rest.substr(0, space));
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    return w;
}

constexpr std::array<std::uint32_t, 64> kSha256K{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::optional<std::vector<std::uint8_t>> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool write_all(const fs::path& path, std::span<const std::uint8_t> bytes) {
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    return !ec;
}

#if defined(_WIN32)
// One argument quoted for CreateProcess (the rules of CommandLineToArgvW)
std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}
#endif

// Runs `program` with `args`, its output and errors into `log`; the exit code, or nothing if it could not start
std::optional<int> run(const fs::path& program, const std::vector<std::string>& args, const fs::path& log) {
#if defined(_WIN32)
    std::wstring command = quote(program.wstring());
    for (const std::string& a : args) {
        command += L" " + quote(fs::path(a).wstring());
    }
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(program.wstring().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!started) {
        return std::nullopt;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : owned) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    const int failed = posix_spawn(&pid, owned[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (failed) {
        return std::nullopt;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
        return std::nullopt;
    }
    return WEXITSTATUS(status);
#endif
}

std::string first_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return std::string(trim(line));
}

}  // namespace

RomList parse_rom_list(std::string_view text) {
    RomList list;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        const std::string kind = word(line);
        if (kind == "site") {
            list.site = std::string(trim(line));
        } else if (kind == "piece") {
            RomListPiece p;
            p.name = word(line);
            p.path = word(line);
            p.sha256 = word(line);
            list.pieces.push_back(std::move(p));
        } else if (kind == "image") {
            RomListImage i;
            i.file = word(line);
            const std::string joined = word(line);
            for (std::size_t start = 0; start <= joined.size();) {
                const auto plus = joined.find('+', start);
                i.pieces.push_back(joined.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
                start = plus == std::string::npos ? joined.size() + 1 : plus + 1;
            }
            i.sha256 = word(line);
            i.crc32 = word(line);
            i.what = std::string(trim(line));
            list.images.push_back(std::move(i));
        } else if (kind == "notice") {
            list.notice += std::string(trim(line)) + "\n";
        }
    }
    return list;
}

const RomList& builtin_rom_list() {
    static const RomList list = parse_rom_list(rom_list_text("roms.txt"));
    return list;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> m(bytes.begin(), bytes.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) {
        m.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    }
    for (std::size_t block = 0; block < m.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t* p = &m[block + static_cast<std::size_t>(i) * 4];
            w[static_cast<std::size_t>(i)] = std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        const std::array<std::uint32_t, 8> add{a, b, c, d, e, f, g, hh};
        for (std::size_t i = 0; i < 8; ++i) {
            h[i] += add[i];
        }
    }
    std::string hex;
    char digits[9];
    for (const std::uint32_t v : h) {
        std::snprintf(digits, sizeof digits, "%08x", static_cast<unsigned>(v));
        hex += digits;
    }
    return hex;
}

CurlCheck find_curl() {
    CurlCheck check;
#if defined(_WIN32)
    const char* name = "curl.exe";
    const char separator = ';';
#else
    const char* name = "curl";
    const char separator = ':';
#endif
    std::vector<fs::path> dirs;
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const auto end = rest.find(separator);
            if (end != 0) {
                dirs.emplace_back(std::string(rest.substr(0, end)));
            }
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
    }
#if defined(_WIN32)
    wchar_t system[MAX_PATH];
    if (const UINT n = GetSystemDirectoryW(system, MAX_PATH); n > 0 && n < MAX_PATH) {
        dirs.emplace_back(system);
    }
#endif
    std::error_code ec;
    for (const fs::path& dir : dirs) {
        const fs::path candidate = dir / name;
        if (fs::is_regular_file(candidate, ec)) {
#if !defined(_WIN32)
            if (access(candidate.c_str(), X_OK) != 0) {
                continue;
            }
#endif
            check.program = candidate;
            break;
        }
    }
    if (check.program.empty()) {
#if defined(_WIN32)
        check.problem = msg("curl was not found (Windows 10 and later have curl.exe in the system folder; install curl or put it on PATH)");
#else
        check.problem = msg("curl was not found on PATH (install it, e.g. `sudo apt install curl`)");
#endif
        return check;
    }
    const fs::path log = fs::temp_directory_path(ec) / "freetvc-curl-version.txt";
    const auto code = run(check.program, {"--version"}, log);
    check.version = first_line(log);
    fs::remove(log, ec);
    if (!code || *code != 0 || check.version.rfind("curl ", 0) != 0) {
        check.problem = check.version.empty() ? msg("%s does not run as curl", check.program.string().c_str())
                                              : msg("%s does not run as curl (%s)", check.program.string().c_str(), check.version.c_str());
    }
    return check;
}

bool fetch_roms(const CurlCheck& curl, const fs::path& dir, std::string& error,
                const std::function<void(const std::string&)>& progress) {
    const RomList& list = builtin_rom_list();
    if (!curl.ok()) {
        error = curl.problem.empty() ? msg("curl is not available") : curl.problem;
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path downloads = dir / ".download";
    fs::create_directories(downloads, ec);
    if (ec) {
        error = msg("cannot create %s: %s", downloads.string().c_str(), ec.message().c_str());
        return false;
    }
    const auto say = [&](const std::string& text) {
        if (progress) {
            progress(text);
        }
    };

    std::map<std::string, std::vector<std::uint8_t>> pieces;
    std::map<std::string, std::string> origin;
    for (const RomListPiece& p : list.pieces) {
        origin[p.name] = p.path;
        const fs::path file = downloads / p.name;
        const std::string url = p.path.rfind("https://", 0) == 0 ? p.path : list.site + "/" + p.path;
        say("downloading " + url);
        // --insecure: the site's certificate chain is incomplete; the pinned SHA-256 vouches for the file instead
        const auto code = run(curl.program,
                              {"--fail", "--silent", "--show-error", "--location", "--insecure", "--max-time", "60", "--output",
                               file.string(), url},
                              downloads / "curl.log");
        auto bytes = read_all(file);
        if (!code || *code != 0 || !bytes) {
            error = code ? msg("downloading %s failed (curl exit %d): %s", url.c_str(), *code, first_line(downloads / "curl.log").c_str())
                         : msg("downloading %s failed: %s", url.c_str(), first_line(downloads / "curl.log").c_str());
            fs::remove_all(downloads, ec);
            return false;
        }
        if (sha256_hex(*bytes) != p.sha256) {
            error = msg("%s: SHA-256 %s, expected %s (the file on the site changed?)", url.c_str(), sha256_hex(*bytes).c_str(), p.sha256.c_str());
            fs::remove_all(downloads, ec);
            return false;
        }
        pieces[p.name] = std::move(*bytes);
    }

    std::string notice = list.notice + "\nfile          CRC-32    what, and the files on " + list.site + " (in socket order)\n";
    for (const RomListImage& image : list.images) {
        std::vector<std::uint8_t> bytes;
        std::string from;
        for (const std::string& piece : image.pieces) {
            // ARCHIVE#MEMBER: a file inside a downloaded ZIP
            const auto hash = piece.find('#');
            const std::string name = piece.substr(0, hash);
            const auto it = pieces.find(name);
            if (it == pieces.end()) {
                error = msg("the ROM list names an unknown piece %s", name.c_str());
                fs::remove_all(downloads, ec);
                return false;
            }
            if (hash == std::string::npos) {
                bytes.insert(bytes.end(), it->second.begin(), it->second.end());
            } else {
                const auto member = unzip_member(it->second, piece.substr(hash + 1));
                if (!member) {
                    error = msg("%s has no readable %s", name.c_str(), piece.substr(hash + 1).c_str());
                    fs::remove_all(downloads, ec);
                    return false;
                }
                bytes.insert(bytes.end(), member->begin(), member->end());
            }
            from += (from.empty() ? "" : " + ") + origin[name] + (hash == std::string::npos ? "" : " (" + piece.substr(hash + 1) + ")");
        }
        if (sha256_hex(bytes) != image.sha256) {
            error = msg("%s: joined SHA-256 %s, expected %s", image.file.c_str(), sha256_hex(bytes).c_str(), image.sha256.c_str());
            fs::remove_all(downloads, ec);
            return false;
        }
        const fs::path target = dir / image.file;
        const auto have = read_all(target);
        if (!have || sha256_hex(*have) != image.sha256) {
            if (!write_all(target, bytes)) {
                error = msg("cannot write %s", target.string().c_str());
                fs::remove_all(downloads, ec);
                return false;
            }
        }
        say("wrote " + target.string());
        notice += image.file + std::string(image.file.size() < 14 ? 14 - image.file.size() : 1, ' ') + image.crc32 + "  " + image.what +
                  ": " + from + "\n";
    }
    const std::string_view text(notice);
    write_all(dir / "ROMS-README.txt", std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    fs::remove_all(downloads, ec);
    return true;
}

}  // namespace freetvc
