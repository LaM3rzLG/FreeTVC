// SPDX-License-Identifier: GPL-3.0-or-later
// Video recording through an ffmpeg program: frames piped as RGB, sound kept in a file, both joined into an MP4 at the end.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/video/framebuffer.hpp"
#include "core/video/video.hpp"

namespace freetvc {

// ffmpeg next to the program (ffmpeg/ffmpeg[.exe]), else in the usual install folders, else on PATH
std::optional<std::filesystem::path> find_ffmpeg(const std::filesystem::path& program_dir);

class ChildProcess;

class VideoRecorder final : public FrameSink {
public:
    VideoRecorder();
    ~VideoRecorder() override;
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // Starts ffmpeg encoding `area` of each frame, doubled, into a temporary file beside `out`; sound at `audio_rate`
    bool start(const std::filesystem::path& out, const std::filesystem::path& ffmpeg, ScreenArea area, unsigned audio_rate,
               std::string& error);
    // Ends the recording and joins picture and sound into `out`; false with `error` set (the temporary files are removed)
    bool stop(std::string& error);
    bool recording() const { return child_ != nullptr; }
    const std::filesystem::path& file() const { return out_; }
    // Recorded machine time
    double seconds() const;

    void frame_done(const Framebuffer& frame) override;
    void audio(std::span<const float> samples);

private:
    void abort();

    std::unique_ptr<ChildProcess> child_;
    std::filesystem::path out_;
    std::filesystem::path ffmpeg_;
    std::filesystem::path video_tmp_;
    std::filesystem::path audio_tmp_;
    std::filesystem::path log_;
    std::FILE* audio_file_ = nullptr;
    ScreenArea area_{};
    unsigned audio_rate_ = 0;
    std::uint64_t frames_ = 0;
    bool failed_ = false;
    std::vector<std::uint8_t> rgb_;
};

}  // namespace freetvc
