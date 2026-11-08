// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/frame_blend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace freetvc::sdl {

namespace {

constexpr double kTvcHz = 3'125'000.0 / 62'800.0;  // 49.76 frames per second
constexpr double kWholeMultiple = 0.03;            // refresh / TVC rate this close to a whole number: no judder to hide

double to_linear(std::uint32_t c) {
    const double v = static_cast<double>(c) / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

std::uint32_t to_srgb(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint32_t>(std::lround(s * 255.0));
}

}  // namespace

bool blends(FrameBlend mode, double refresh_hz) {
    if (mode != FrameBlend::Auto) {
        return mode == FrameBlend::On;
    }
    if (refresh_hz <= 0.0) {
        return false;
    }
    const double ratio = refresh_hz / kTvcHz;
    return ratio >= 1.0 - kWholeMultiple && std::abs(ratio - std::round(ratio)) > kWholeMultiple;
}

float shaped_mix(float mix, int sharpness) {
    const float width = 1.0f - static_cast<float>(std::clamp(sharpness, 0, 100)) / 100.0f;
    if (width < 0.01f) {
        return mix < 0.5f ? 0.0f : 1.0f;
    }
    return std::clamp((mix - 0.5f) / width + 0.5f, 0.0f, 1.0f);
}

void blend_frames(const Framebuffer& a, const Framebuffer& b, float mix, std::uint32_t* out) {
    constexpr std::size_t n = Framebuffer::kColours;
    std::array<std::array<double, 3>, n> linear{};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            linear[i][c] = to_linear((kRgbPalette[i] >> (16 - 8 * c)) & 0xFFu);
        }
    }
    // 17 x 17 colour pairs: the mix of each pair once, then a lookup per pixel
    std::array<std::uint32_t, n * n> pair{};
    const double m = std::clamp(static_cast<double>(mix), 0.0, 1.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            std::uint32_t argb = 0xFF000000u;
            for (std::size_t c = 0; c < 3; ++c) {
                argb |= to_srgb(linear[i][c] * (1.0 - m) + linear[j][c] * m) << (16 - 8 * c);
            }
            pair[i * n + j] = argb;
        }
    }
    const std::size_t count = static_cast<std::size_t>(Framebuffer::kWidth) * Framebuffer::kHeight;
    for (std::size_t p = 0; p < count; ++p) {
        const std::size_t i = a.pixels[p] < n ? a.pixels[p] : 0;
        const std::size_t j = b.pixels[p] < n ? b.pixels[p] : 0;
        out[p] = pair[i * n + j];
    }
}

}  // namespace freetvc::sdl
