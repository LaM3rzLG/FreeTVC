// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/video_recorder.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/message.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace freetvc {

namespace {

// 3 125 000 Hz / 62 800 T per frame (tvc-hardware.md §2)
constexpr const char* kFrameRate = "3125000/62800";
constexpr unsigned kFrameTicks = 62800;
constexpr double kClock = 3125000.0;

// Arguments are UTF-8 on every system
std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string last_lines(const fs::path& log) {
    std::ifstream in(log);
    std::string line;
    std::string tail;
    std::array<std::string, 3> keep;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            keep[n++ % keep.size()] = line;
        }
    }
    for (std::size_t i = n > keep.size() ? n - keep.size() : 0; i < n; ++i) {
        tail += (tail.empty() ? "" : " / ") + keep[i % keep.size()];
    }
    return tail;
}

}  // namespace

// ffmpeg with its standard input as a pipe (or none) and its output into a log file
class ChildProcess {
public:
    ~ChildProcess() { wait(); }

    bool start(const std::vector<std::string>& args, bool pipe_in, const fs::path& log, std::string& error) {
#ifdef _WIN32
        std::wstring cmd;
        for (const std::string& a : args) {
            std::wstring w(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), w.data(), static_cast<int>(w.size()));
            std::wstring q = L"\"";
            for (std::size_t i = 0; i < w.size(); ++i) {
                std::size_t slashes = 0;
                while (i < w.size() && w[i] == L'\\') {
                    ++slashes;
                    ++i;
                }
                if (i == w.size()) {
                    q.append(slashes * 2, L'\\');
                    break;
                }
                if (w[i] == L'"') {
                    q.append(slashes * 2 + 1, L'\\');
                } else {
                    q.append(slashes, L'\\');
                }
                q += w[i];
            }
            q += L"\"";
            cmd += (cmd.empty() ? L"" : L" ") + q;
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE read_end = nullptr;
        if (pipe_in) {
            if (!CreatePipe(&read_end, &write_, &sa, 1 << 20)) {
                error = msg("cannot make a pipe for ffmpeg");
                return false;
            }
            SetHandleInformation(write_, HANDLE_FLAG_INHERIT, 0);
        }
        HANDLE log_handle = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = read_end;
        si.hStdOutput = log_handle;
        si.hStdError = log_handle;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (read_end) {
            CloseHandle(read_end);
        }
        if (log_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(log_handle);
        }
        if (!ok) {
            error = msg("cannot start ffmpeg (%s)", args[0].c_str());
            close_input();
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        return true;
#else
        signal(SIGPIPE, SIG_IGN);  // a stopped ffmpeg ends the recording, not the emulator
        int fds[2] = {-1, -1};
        if (pipe_in && pipe(fds) != 0) {
            error = msg("cannot make a pipe for ffmpeg");
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (pipe_in) {
            posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
            posix_spawn_file_actions_addclose(&actions, fds[0]);
            posix_spawn_file_actions_addclose(&actions, fds[1]);
        } else {
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        }
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        std::vector<char*> argv;
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid_, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (pipe_in) {
            close(fds[0]);
        }
        if (rc != 0) {
            if (pipe_in) {
                close(fds[1]);
            }
            pid_ = -1;
            error = msg("cannot start ffmpeg (%s)", args[0].c_str());
            return false;
        }
        write_ = fds[1];
        return true;
#endif
    }

    bool write(const void* data, std::size_t size) {
#ifdef _WIN32
        const auto* p = static_cast<const char*>(data);
        while (size > 0 && write_) {
            DWORD done = 0;
            if (!WriteFile(write_, p, static_cast<DWORD>(size), &done, nullptr)) {
                return false;
            }
            p += done;
            size -= done;
        }
        return size == 0;
#else
        const auto* p = static_cast<const char*>(data);
        while (size > 0 && write_ >= 0) {
            const ssize_t done = ::write(write_, p, size);
            if (done <= 0) {
                return false;
            }
            p += done;
            size -= static_cast<std::size_t>(done);
        }
        return size == 0;
#endif
    }

    void close_input() {
#ifdef _WIN32
        if (write_) {
            CloseHandle(write_);
            write_ = nullptr;
        }
#else
        if (write_ >= 0) {
            close(write_);
            write_ = -1;
        }
#endif
    }

    // The exit code, -1 when it did not exit normally
    int wait() {
        close_input();
#ifdef _WIN32
        if (!process_) {
            return code_;
        }
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process_, &code);
        CloseHandle(process_);
        process_ = nullptr;
        code_ = static_cast<int>(code);
#else
        if (pid_ <= 0) {
            return code_;
        }
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
        return code_;
    }

private:
#ifdef _WIN32
    HANDLE process_ = nullptr;
    HANDLE write_ = nullptr;
#else
    pid_t pid_ = -1;
    int write_ = -1;
#endif
    int code_ = -1;
};

std::optional<fs::path> find_ffmpeg(const fs::path& program_dir) {
#ifdef _WIN32
    const char* name = "ffmpeg.exe";
    const char sep = ';';
#else
    const char* name = "ffmpeg";
    const char sep = ':';
#endif
    std::error_code ec;
    std::vector<fs::path> places{program_dir / "ffmpeg" / name, program_dir / name};
#ifdef __APPLE__
    places.push_back("/opt/homebrew/bin/ffmpeg");
    places.push_back("/usr/local/bin/ffmpeg");
#endif
    if (const char* path = std::getenv("PATH")) {
        std::stringstream dirs(path);
        std::string dir;
        while (std::getline(dirs, dir, sep)) {
            if (!dir.empty()) {
                places.push_back(fs::path(dir) / name);
            }
        }
    }
    for (const fs::path& p : places) {
        if (fs::is_regular_file(p, ec)) {
            return p;
        }
    }
    return std::nullopt;
}

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder() { abort(); }

bool VideoRecorder::start(const fs::path& out, const fs::path& ffmpeg, ScreenArea area, unsigned audio_rate, std::string& error) {
    abort();
    out_ = out;
    ffmpeg_ = ffmpeg;
    area_ = area;
    audio_rate_ = audio_rate;
    frames_ = 0;
    failed_ = false;
    video_tmp_ = fs::path(out).concat(".video.mp4");
    audio_tmp_ = fs::path(out).concat(".audio.raw");
    log_ = fs::path(out).concat(".ffmpeg.log");
#ifdef _WIN32
    audio_file_ = _wfopen(audio_tmp_.wstring().c_str(), L"wb");
#else
    audio_file_ = std::fopen(audio_tmp_.c_str(), "wb");
#endif
    if (!audio_file_) {
        error = msg("cannot write '%s'", utf8(audio_tmp_).c_str());
        return false;
    }
    // Pixels doubled with nearest neighbour, so the 4:2:0 colour keeps every TVC pixel sharp
    const std::string size = std::to_string(area.width) + "x" + std::to_string(area.height);
    const std::string scale = "scale=" + std::to_string(area.width * 2) + ":" + std::to_string(area.height * 2) + ":flags=neighbor";
    const std::vector<std::string> args{utf8(ffmpeg), "-hide_banner", "-loglevel", "error", "-y",
                                        "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", size,
                                        "-framerate", kFrameRate, "-i", "-",
                                        "-vf", scale, "-c:v", "libx264", "-preset", "veryfast", "-crf", "16",
                                        "-pix_fmt", "yuv420p", utf8(video_tmp_)};
    auto child = std::make_unique<ChildProcess>();
    if (!child->start(args, true, log_, error)) {
        abort();
        return false;
    }
    child_ = std::move(child);
    rgb_.resize(static_cast<std::size_t>(area.width) * area.height * 3);
    return true;
}

void VideoRecorder::frame_done(const Framebuffer& frame) {
    if (!child_ || failed_) {
        return;
    }
    std::uint8_t* p = rgb_.data();
    for (int y = area_.y; y < area_.y + area_.height; ++y) {
        const std::uint8_t* row = &frame.pixels[static_cast<std::size_t>(y) * Framebuffer::kWidth + area_.x];
        for (int x = 0; x < area_.width; ++x) {
            const std::uint32_t c = kRgbPalette[row[x]];
            *p++ = static_cast<std::uint8_t>(c >> 16);
            *p++ = static_cast<std::uint8_t>(c >> 8);
            *p++ = static_cast<std::uint8_t>(c);
        }
    }
    if (!child_->write(rgb_.data(), rgb_.size())) {
        failed_ = true;
        return;
    }
    ++frames_;
}

void VideoRecorder::audio(std::span<const float> samples) {
    if (audio_file_ && !samples.empty()) {
        std::fwrite(samples.data(), sizeof(float), samples.size(), audio_file_);
    }
}

double VideoRecorder::seconds() const { return static_cast<double>(frames_) * kFrameTicks / kClock; }

bool VideoRecorder::stop(std::string& error) {
    if (!child_) {
        error = msg("no video is being recorded");
        return false;
    }
    std::fclose(audio_file_);
    audio_file_ = nullptr;
    const int code = child_->wait();
    child_.reset();
    std::error_code ec;
    bool ok = !failed_ && code == 0 && frames_ > 0;
    if (!ok) {
        error = frames_ == 0 && code == 0 ? msg("no frame was recorded")
                                          : msg("ffmpeg failed: %s", last_lines(log_).c_str());
    } else {
        const std::vector<std::string> args{utf8(ffmpeg_), "-hide_banner", "-loglevel", "error", "-y",
                                            "-i", utf8(video_tmp_),
                                            "-f", "f32le", "-ar", std::to_string(audio_rate_), "-ac", "1", "-i", utf8(audio_tmp_),
                                            "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                                            "-movflags", "+faststart", utf8(out_)};
        ChildProcess mux;
        ok = mux.start(args, false, log_, error);
        if (ok && mux.wait() != 0) {
            error = msg("ffmpeg failed: %s", last_lines(log_).c_str());
            ok = false;
        }
    }
    fs::remove(video_tmp_, ec);
    fs::remove(audio_tmp_, ec);
    if (ok) {
        fs::remove(log_, ec);
    }
    return ok;
}

void VideoRecorder::abort() {
    if (audio_file_) {
        std::fclose(audio_file_);
        audio_file_ = nullptr;
    }
    if (child_) {
        child_->wait();
        child_.reset();
        std::error_code ec;
        fs::remove(video_tmp_, ec);
        fs::remove(audio_tmp_, ec);
    }
}

}  // namespace freetvc
