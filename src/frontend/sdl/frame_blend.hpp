// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "core/video/framebuffer.hpp"

namespace freetvc::sdl {

// Frame blending (design/ui.md "Frame pacing"): off, on, or on where the display's rate is no whole multiple of the TVC's
enum class FrameBlend { Auto, On, Off };

// True when `mode` blends on a display refreshing at `refresh_hz` (0 = unknown: no blending in Auto).
bool blends(FrameBlend mode, double refresh_hz);

// The next frame's share at the clock's `mix` (0-1): `sharpness` 0 mixes all the way, 100 never (percent);
// in between only the middle of the way is mixed and each frame stays alone around its own time
float shaped_mix(float mix, int sharpness);

// ARGB8888 pixels of `a` and `b` mixed in linear light, `mix` = b's share (0-1); `out` holds kWidth * kHeight pixels.
void blend_frames(const Framebuffer& a, const Framebuffer& b, float mix, std::uint32_t* out);

}  // namespace freetvc::sdl
