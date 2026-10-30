// SPDX-License-Identifier: GPL-3.0-or-later
// GLSL 3.30 for the picture model (design/ui.md "Picture"), written from the PAL signal: the TVC's pixels at 12.5 MHz,
// the subcarrier at 4.43361875 MHz, signal passes sampled at 25 MHz (two samples a TVC pixel), offscreen rows = raster lines.
#pragma once

namespace freetvc::sdl::shaders {

// One triangle over the whole target; no vertex buffer
inline constexpr const char* kVertex = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Shared by the signal passes: the composite signal the TVC's encoder makes from its pixels
inline constexpr const char* kSignalCommon = R"(#version 330 core
uniform sampler2D u_src;       // the framebuffer, 800 x 314, nearest
uniform float u_frame_phase;   // subcarrier cycles at the frame's first line
uniform float u_colour;        // 1 = subcarrier on, 0 = the rear switch on B/W
const float kCyclesPerPixel = 0.354689500;  // 4.43361875 MHz / 12.5 MHz
const float kCyclesPerLine = 0.7516;        // 283.7516 cycles a 64 us line, whole cycles dropped
out vec4 o_colour;

vec3 yuv_of(vec3 rgb) {
    float y = dot(rgb, vec3(0.299, 0.587, 0.114));
    return vec3(y, 0.492 * (rgb.b - y), 0.877 * (rgb.r - y));
}
float v_switch(float line) { return mod(line, 2.0) < 1.0 ? 1.0 : -1.0; }  // PAL: V inverted every other line
float phase(float x, float line) { return 6.28318531 * fract(u_frame_phase + line * kCyclesPerLine + x * kCyclesPerPixel); }
// x in TVC pixels from the line's start
float composite(float x, float line) {
    vec3 rgb = texelFetch(u_src, ivec2(clamp(int(floor(x)), 0, 799), int(line)), 0).rgb;
    vec3 yuv = yuv_of(rgb);
    float p = phase(x, line);
    return yuv.x + u_colour * (yuv.y * sin(p) + v_switch(line) * yuv.z * cos(p));
}
float gauss(float k, float sigma) { return exp(-0.5 * k * k / (sigma * sigma)); }
)";

// Pass 1, 1600 x 314 RG: U and V demodulated with the set's (phase-shifted) subcarrier and low-passed
inline constexpr const char* kChroma = R"(
uniform float u_sigma;        // chroma low-pass, in samples
uniform float u_phase_error;  // radians
void main() {
    float line = gl_FragCoord.y - 0.5;
    float x0 = gl_FragCoord.x * 0.5;
    int reach = int(ceil(u_sigma * 3.0));
    vec2 sum = vec2(0.0);
    float weights = 0.0;
    for (int k = -reach; k <= reach; ++k) {
        float x = x0 + float(k) * 0.5;
        float w = gauss(float(k), u_sigma);
        float p = phase(x, line) + u_phase_error;
        sum += w * 2.0 * composite(x, line) * vec2(sin(p), v_switch(line) * cos(p));
        weights += w;
    }
    o_colour = vec4(sum / weights, 0.0, 1.0);
}
)";

// Pass 2, 1600 x 314 RGBA: Y through the set's luma path (the chroma trap takes out the remodulated chroma on a colour set),
// U and V through the delay line
inline constexpr const char* kDecode = R"(
uniform sampler2D u_uv;
uniform float u_sigma;        // luma low-pass, in samples
uniform float u_trap;         // 1 = colour set
uniform float u_delay;        // 1 = PAL delay line
uniform float u_saturation;
uniform float u_phase_error;
void main() {
    float line = gl_FragCoord.y - 0.5;
    int i = int(gl_FragCoord.x);
    float x0 = gl_FragCoord.x * 0.5;
    int reach = max(1, int(ceil(u_sigma * 3.0)));
    float y = 0.0;
    float weights = 0.0;
    for (int k = -reach; k <= reach; ++k) {
        float x = x0 + float(k) * 0.5;
        float c = composite(x, line);
        if (u_trap > 0.5) {
            vec2 uv = texelFetch(u_uv, ivec2(clamp(i + k, 0, 1599), int(line)), 0).rg;
            float p = phase(x, line) + u_phase_error;
            c -= uv.x * sin(p) + v_switch(line) * uv.y * cos(p);
        }
        float w = gauss(float(k), u_sigma);
        y += w * c;
        weights += w;
    }
    vec2 uv = texelFetch(u_uv, ivec2(i, int(line)), 0).rg;
    if (u_delay > 0.5 && line > 0.5) {
        uv = 0.5 * (uv + texelFetch(u_uv, ivec2(i, int(line) - 1), 0).rg);
    }
    o_colour = vec4(y / weights, uv * u_saturation * u_trap, 1.0);
}
)";

// Shared by the glow passes and the tube: the signal (RGB pixels or decoded YUV) as light
inline constexpr const char* kLightCommon = R"(#version 330 core
uniform sampler2D u_sig;   // the framebuffer (RGB) or pass 2 (YUV)
uniform int u_yuv;
uniform float u_gamma;     // the tube's
out vec4 o_colour;
vec3 rgb_of(vec3 yuv) {
    return vec3(yuv.x + 1.13983 * yuv.z, yuv.x - 0.39465 * yuv.y - 0.58060 * yuv.z, yuv.x + 2.03211 * yuv.y);
}
vec3 light(vec3 v) { return pow(clamp(v, 0.0, 1.0), vec3(u_gamma)); }
// x in TVC pixels, line a raster line; linear filtering along the line, none across
vec3 signal_light(float x, float line) {
    vec4 s = texture(u_sig, vec2(x / 800.0, (clamp(line, 0.0, 313.0) + 0.5) / 314.0));
    return light(u_yuv == 1 ? rgb_of(s.xyz) : s.rgb);
}
)";

// Glow, two passes into 400 x 314 and 400 x 157: a wide blur of the light
inline constexpr const char* kGlowH = R"(
void main() {
    float x = gl_FragCoord.x * 2.0;
    float line = gl_FragCoord.y - 0.5;
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int k = -12; k <= 12; ++k) {
        float w = exp(-0.5 * float(k * k) / 36.0);
        sum += w * signal_light(x + float(k) * 2.0, line);
        weights += w;
    }
    o_colour = vec4(sum / weights, 1.0);
}
)";
inline constexpr const char* kGlowV = R"(#version 330 core
uniform sampler2D u_glow;
out vec4 o_colour;
void main() {
    vec2 at = vec2(gl_FragCoord.x / 400.0, gl_FragCoord.y * 2.0);
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int k = -8; k <= 8; ++k) {
        float w = exp(-0.5 * float(k * k) / 16.0);
        sum += w * texture(u_glow, vec2(at.x, (clamp(at.y + float(k), 0.5, 313.5)) / 314.0)).rgb;
        weights += w;
    }
    o_colour = vec4(sum / weights, 1.0);
}
)";

// The tube, drawn over the screen's rectangle in the window
inline constexpr const char* kTube = R"(
uniform vec4 u_rect;       // the picture in window pixels: x, y from the bottom, width, height
uniform vec4 u_area;       // the framebuffer area shown: x, y, width, height
uniform vec2 u_beam;       // vertical sigma for black and for white, in line pitches
uniform float u_line_px;   // window pixels a raster line takes
uniform float u_spot;      // horizontal sigma in TVC pixels (RGB)
uniform int u_mask;        // 0 none, 1 aperture grille, 2 shadow mask
uniform float u_mask_strength;
uniform float u_mask_pitch;
uniform sampler2D u_glow;
uniform float u_glow_amount;
uniform float u_curvature;
uniform float u_vignette;
uniform float u_brightness;

vec3 beam_light(float x, float line) {
    if (u_yuv == 1) {
        return signal_light(x, line);  // already band-limited
    }
    float centre = floor(x);
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int j = -2; j <= 2; ++j) {
        float px = centre + float(j);
        float d = px + 0.5 - x;
        float w = exp(-0.5 * d * d / (u_spot * u_spot));
        sum += w * light(texelFetch(u_sig, ivec2(clamp(int(px), 0, 799), int(clamp(line, 0.0, 313.0))), 0).rgb);
        weights += w;
    }
    return sum / weights;
}

vec3 mask(vec2 at) {
    if (u_mask == 0) {
        return vec3(1.0);
    }
    float x = at.x;
    if (u_mask == 2) {
        x += mod(floor(at.y / (u_mask_pitch * 0.5)), 2.0) * u_mask_pitch * 0.5;  // the triads' rows interleave
    }
    int stripe = int(mod(x / u_mask_pitch, 1.0) * 3.0);
    vec3 m = vec3(1.0 - u_mask_strength);
    m[stripe] = 1.0;
    return m / (1.0 - u_mask_strength * 2.0 / 3.0);
}

void main() {
    vec2 p = (gl_FragCoord.xy - u_rect.xy) / u_rect.zw;
    p.y = 1.0 - p.y;
    vec2 c = p * 2.0 - 1.0;
    c *= 1.0 + u_curvature * c.yx * c.yx;
    c /= 1.0 + u_curvature;
    if (abs(c.x) > 1.0 || abs(c.y) > 1.0) {
        o_colour = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    p = c * 0.5 + 0.5;
    float x = u_area.x + p.x * u_area.z;
    float y = u_area.y + p.y * u_area.w;
    float first = floor(y - 0.5);
    vec3 sum = vec3(0.0);
    for (int n = -1; n <= 2; ++n) {
        float line = first + float(n);
        vec3 l = beam_light(x, line);
        float sigma = mix(u_beam.x, u_beam.y, sqrt(clamp(dot(l, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0)));
        sigma = max(sigma, 0.6 / u_line_px);  // no narrower than the window's pixels can show, or the lines alias
        float d = y - (line + 0.5);
        sum += l * exp(-0.5 * d * d / (sigma * sigma)) / (sigma * 2.50662827);
    }
    if (u_glow_amount > 0.0) {
        sum += u_glow_amount * texture(u_glow, vec2(x / 800.0, y / 314.0)).rgb;
    }
    sum *= mask(gl_FragCoord.xy) * u_brightness * (1.0 - u_vignette * 0.5 * dot(c, c) * dot(c, c));
    o_colour = vec4(pow(clamp(sum, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
)";

}  // namespace freetvc::sdl::shaders
