// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenGL 3.3 display with the picture model (design/ui.md "Picture").
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include "frontend/sdl/display_impl.hpp"
#include "frontend/sdl/gl_api.hpp"
#include "frontend/sdl/picture_shaders.hpp"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace freetvc::sdl {

namespace {

using namespace gl;

constexpr int kWidth = Framebuffer::kWidth;    // 800
constexpr int kLines = Framebuffer::kHeight;   // 314
constexpr int kSamples = kWidth * 2;           // the signal passes' 25 MHz
constexpr int kGlowWidth = kWidth / 2;
constexpr int kGlowLines = kLines / 2;
constexpr double kSamplesPerMicrosecond = 25.0;
constexpr double kPi = 3.14159265358979323846;

// The sigma, in 25 MHz samples, of a Gaussian low-pass that is 3 dB down at `mhz`
float sigma_samples(float mhz) { return static_cast<float>(std::sqrt(std::log(2.0)) / (2.0 * kPi * mhz) * kSamplesPerMicrosecond); }

struct Target {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int width = 0;
    int height = 0;
};

struct TubeCall {
    void* display;
    float rect[4];  // window pixels: x, y, width, height (ImGui's coordinates, y down)
    float area[4];
};

class GlDisplay final : public Display {
public:
    GlDisplay(SDL_Window* window, SDL_GLContext context, const Api& api) : context_(context), gl_(api) {
        window_ = window;
        name_ = std::string("OpenGL ") + reinterpret_cast<const char*>(gl_.GetString(VERSION)) + " (" +
                reinterpret_cast<const char*>(gl_.GetString(RENDERER)) + ")";
        ImGui_ImplSDL3_InitForOpenGL(window, context);
        ImGui_ImplOpenGL3_Init("#version 330 core");
        gl_.GenVertexArrays(1, &vao_);
        build_programs();
        uv_ = make_target(kSamples, kLines, RG16F, RG, LINEAR);
        yuv_ = make_target(kSamples, kLines, RGBA16F, RGBA, LINEAR);
        glow_h_ = make_target(kGlowWidth, kLines, RGBA16F, RGBA, LINEAR);
        glow_v_ = make_target(kGlowWidth, kGlowLines, RGBA16F, RGBA, LINEAR);
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
    }
    ~GlDisplay() override {
        for (Target* t : {&uv_, &yuv_, &glow_h_, &glow_v_}) {
            gl_.DeleteFramebuffers(1, &t->framebuffer);
            gl_.DeleteTextures(1, &t->texture);
        }
        for (GLuint program : {chroma_, decode_, glow_h_program_, glow_v_program_, tube_}) {
            gl_.DeleteProgram(program);
        }
        gl_.DeleteVertexArrays(1, &vao_);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        SDL_GL_DestroyContext(context_);
        SDL_DestroyWindow(window_);
    }

    std::string_view name() const override { return name_; }
    bool effects() const override { return shader_error_.empty(); }

    ImTextureID create_texture(int width, int height) override {
        GLuint texture = 0;
        gl_.GenTextures(1, &texture);
        gl_.BindTexture(TEXTURE_2D, texture);
        set_filter(NEAREST);
        gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, width, height, 0, BGRA, UNSIGNED_BYTE, nullptr);
        sizes_[texture] = {width, height};
        return static_cast<ImTextureID>(texture);
    }
    void update_texture(ImTextureID texture, const void* pixels, int pitch) override {
        const GLuint name = static_cast<GLuint>(texture);
        const auto found = sizes_.find(name);
        if (found == sizes_.end()) {
            return;
        }
        gl_.BindTexture(TEXTURE_2D, name);
        gl_.PixelStorei(UNPACK_ROW_LENGTH, pitch / 4);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
        gl_.TexSubImage2D(TEXTURE_2D, 0, 0, 0, found->second.first, found->second.second, BGRA, UNSIGNED_BYTE, pixels);
        gl_.PixelStorei(UNPACK_ROW_LENGTH, 0);
    }
    void destroy_texture(ImTextureID texture) override {
        const GLuint name = static_cast<GLuint>(texture);
        gl_.DeleteTextures(1, &name);
        sizes_.erase(name);
    }
    void set_vsync(bool on) override { SDL_GL_SetSwapInterval(on ? 1 : 0); }

    void new_frame() override {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
    }
    void present(Colour clear) override {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        gl_.Viewport(0, 0, w, h);
        gl_.ClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f);
        gl_.Clear(COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
    }

    bool set_picture(const Picture& picture, std::string& error) override {
        picture_ = picture;
        error = shader_error_;
        return shader_error_.empty();
    }

    void prepare_picture(ImTextureID screen, bool colour, std::uint64_t frame) override {
        screen_ = static_cast<GLuint>(screen);
        if (!effects() || picture_.signal == Signal::Sharp) {
            return;
        }
        const bool pal = picture_.signal == Signal::Pal || picture_.signal == Signal::Mono;
        gl_.Disable(SCISSOR_TEST);
        gl_.Disable(BLEND);
        gl_.BindVertexArray(vao_);
        gl_.ActiveTexture(TEXTURE0);
        if (pal) {
            // The frame's subcarrier phase: 314 lines of 283.7516 cycles each, whole cycles dropped
            const double phase = std::fmod(static_cast<double>(frame % 1'000'000) * kLines * 0.7516, 1.0);
            const float error = picture_.phase_error * static_cast<float>(kPi / 180.0);
            begin_pass(uv_, chroma_);
            bind(chroma_, "u_src", 0, screen_);
            set(chroma_, "u_frame_phase", static_cast<float>(phase));
            set(chroma_, "u_colour", colour ? 1.0f : 0.0f);
            set(chroma_, "u_sigma", sigma_samples(picture_.chroma_mhz));
            set(chroma_, "u_phase_error", error);
            gl_.DrawArrays(TRIANGLES, 0, 3);

            begin_pass(yuv_, decode_);
            bind(decode_, "u_src", 0, screen_);
            bind(decode_, "u_uv", 1, uv_.texture);
            set(decode_, "u_frame_phase", static_cast<float>(phase));
            set(decode_, "u_colour", colour ? 1.0f : 0.0f);
            set(decode_, "u_sigma", sigma_samples(picture_.luma_mhz));
            // No burst (the rear switch on B/W): the set's colour killer turns its chroma path and trap off
            set(decode_, "u_trap", picture_.signal == Signal::Pal && colour ? 1.0f : 0.0f);
            set(decode_, "u_delay", picture_.delay_line ? 1.0f : 0.0f);
            set(decode_, "u_saturation", picture_.saturation);
            set(decode_, "u_phase_error", error);
            gl_.DrawArrays(TRIANGLES, 0, 3);
        }
        if (picture_.glow > 0.0f) {
            begin_pass(glow_h_, glow_h_program_);
            bind_signal(glow_h_program_, pal);
            gl_.DrawArrays(TRIANGLES, 0, 3);
            begin_pass(glow_v_, glow_v_program_);
            bind(glow_v_program_, "u_glow", 0, glow_h_.texture);
            gl_.DrawArrays(TRIANGLES, 0, 3);
        }
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
        gl_.UseProgram(0);
    }

    bool draw_picture(ImDrawList* draw, ImVec2 min, ImVec2 max, ScreenArea area) override {
        if (!effects() || picture_.signal == Signal::Sharp) {
            return false;
        }
        TubeCall call{this,
                      {min.x, min.y, max.x - min.x, max.y - min.y},
                      {static_cast<float>(area.x), static_cast<float>(area.y), static_cast<float>(area.width), static_cast<float>(area.height)}};
        draw->AddCallback(&GlDisplay::tube_callback, &call, sizeof call);
        draw->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        return true;
    }

private:
    static void tube_callback(const ImDrawList*, const ImDrawCmd* cmd) {
        const auto* call = static_cast<const TubeCall*>(cmd->UserCallbackData);
        static_cast<GlDisplay*>(call->display)->draw_tube(*call, cmd->ClipRect);
    }

    void draw_tube(const TubeCall& call, ImVec4 clip) {
        const ImDrawData* data = ImGui::GetDrawData();
        const ImVec2 scale = data->FramebufferScale;
        const ImVec2 origin = data->DisplayPos;
        const float fb_h = data->DisplaySize.y * scale.y;
        const float x = (call.rect[0] - origin.x) * scale.x;
        const float y = fb_h - (call.rect[1] + call.rect[3] - origin.y) * scale.y;
        const float w = call.rect[2] * scale.x;
        const float h = call.rect[3] * scale.y;
        // Only the picture's pixels inside the list's clip rectangle
        const float cx0 = std::max(call.rect[0], clip.x);
        const float cy0 = std::max(call.rect[1], clip.y);
        const float cx1 = std::min(call.rect[0] + call.rect[2], clip.z);
        const float cy1 = std::min(call.rect[1] + call.rect[3], clip.w);
        if (cx1 <= cx0 || cy1 <= cy0) {
            return;
        }
        gl_.Enable(SCISSOR_TEST);
        gl_.Scissor(static_cast<GLint>((cx0 - origin.x) * scale.x), static_cast<GLint>(fb_h - (cy1 - origin.y) * scale.y),
                    static_cast<GLsizei>((cx1 - cx0) * scale.x), static_cast<GLsizei>((cy1 - cy0) * scale.y));
        gl_.Disable(BLEND);
        gl_.BindVertexArray(vao_);
        gl_.UseProgram(tube_);
        const bool pal = picture_.signal == Signal::Pal || picture_.signal == Signal::Mono;
        bind_signal(tube_, pal);
        bind(tube_, "u_glow", 1, glow_v_.texture);
        set4(tube_, "u_rect", x, y, w, h);
        set4(tube_, "u_area", call.area[0], call.area[1], call.area[2], call.area[3]);
        gl_.Uniform2f(gl_.GetUniformLocation(tube_, "u_beam"), picture_.beam_min, picture_.beam_max);
        set(tube_, "u_line_px", h / call.area[3]);
        set(tube_, "u_spot", picture_.spot);
        gl_.Uniform1i(gl_.GetUniformLocation(tube_, "u_mask"), static_cast<GLint>(picture_.mask));
        set(tube_, "u_mask_strength", picture_.mask_strength);
        set(tube_, "u_mask_pitch", picture_.mask_pitch * scale.x);
        set(tube_, "u_glow_amount", picture_.glow);
        set(tube_, "u_curvature", picture_.curvature);
        set(tube_, "u_vignette", picture_.vignette);
        set(tube_, "u_brightness", picture_.brightness);
        gl_.DrawArrays(TRIANGLES, 0, 3);
        gl_.ActiveTexture(TEXTURE0);
    }

    // The light the glow and the tube read: the framebuffer's RGB, or pass 2's YUV
    void bind_signal(GLuint program, bool pal) {
        gl_.UseProgram(program);
        bind(program, "u_sig", 0, pal ? yuv_.texture : screen_);
        gl_.Uniform1i(gl_.GetUniformLocation(program, "u_yuv"), pal ? 1 : 0);
        set(program, "u_gamma", picture_.crt_gamma);
    }

    void begin_pass(const Target& target, GLuint program) {
        gl_.BindFramebuffer(FRAMEBUFFER, target.framebuffer);
        gl_.Viewport(0, 0, target.width, target.height);
        gl_.UseProgram(program);
    }
    void bind(GLuint program, const char* name, int unit, GLuint texture) {
        gl_.ActiveTexture(TEXTURE0 + static_cast<GLenum>(unit));
        gl_.BindTexture(TEXTURE_2D, texture);
        gl_.Uniform1i(gl_.GetUniformLocation(program, name), unit);
    }
    void set(GLuint program, const char* name, float value) { gl_.Uniform1f(gl_.GetUniformLocation(program, name), value); }
    void set4(GLuint program, const char* name, float a, float b, float c, float d) {
        gl_.Uniform4f(gl_.GetUniformLocation(program, name), a, b, c, d);
    }
    void set_filter(GLint filter) {
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    }

    Target make_target(int width, int height, GLint format, GLenum layout, GLint filter) {
        Target t{0, 0, width, height};
        gl_.GenTextures(1, &t.texture);
        gl_.BindTexture(TEXTURE_2D, t.texture);
        set_filter(filter);
        gl_.TexImage2D(TEXTURE_2D, 0, format, width, height, 0, layout, FLOAT, nullptr);
        gl_.GenFramebuffers(1, &t.framebuffer);
        gl_.BindFramebuffer(FRAMEBUFFER, t.framebuffer);
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, t.texture, 0);
        if (gl_.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE && shader_error_.empty()) {
            shader_error_ = "an offscreen target is incomplete";
        }
        return t;
    }

    GLuint compile(GLenum kind, const std::vector<const char*>& parts) {
        const GLuint shader = gl_.CreateShader(kind);
        gl_.ShaderSource(shader, static_cast<GLsizei>(parts.size()), parts.data(), nullptr);
        gl_.CompileShader(shader);
        GLint ok = 0;
        gl_.GetShaderiv(shader, COMPILE_STATUS, &ok);
        if (!ok && shader_error_.empty()) {
            std::array<char, 2048> log{};
            gl_.GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            shader_error_ = std::string("shader: ") + log.data();
        }
        return shader;
    }
    GLuint program(const std::vector<const char*>& fragment) {
        const GLuint vs = compile(VERTEX_SHADER, {shaders::kVertex});
        const GLuint fs = compile(FRAGMENT_SHADER, fragment);
        const GLuint p = gl_.CreateProgram();
        gl_.AttachShader(p, vs);
        gl_.AttachShader(p, fs);
        gl_.LinkProgram(p);
        GLint ok = 0;
        gl_.GetProgramiv(p, LINK_STATUS, &ok);
        if (!ok && shader_error_.empty()) {
            std::array<char, 2048> log{};
            gl_.GetProgramInfoLog(p, static_cast<GLsizei>(log.size()), nullptr, log.data());
            shader_error_ = std::string("shader link: ") + log.data();
        }
        gl_.DeleteShader(vs);
        gl_.DeleteShader(fs);
        return p;
    }
    void build_programs() {
        chroma_ = program({shaders::kSignalCommon, shaders::kChroma});
        decode_ = program({shaders::kSignalCommon, shaders::kDecode});
        glow_h_program_ = program({shaders::kLightCommon, shaders::kGlowH});
        glow_v_program_ = program({shaders::kGlowV});
        tube_ = program({shaders::kLightCommon, shaders::kTube});
    }

    SDL_GLContext context_;
    Api gl_;
    std::string name_;
    std::string shader_error_;
    Picture picture_;
    GLuint vao_ = 0;
    GLuint screen_ = 0;
    GLuint chroma_ = 0;
    GLuint decode_ = 0;
    GLuint glow_h_program_ = 0;
    GLuint glow_v_program_ = 0;
    GLuint tube_ = 0;
    Target uv_;
    Target yuv_;
    Target glow_h_;
    Target glow_v_;
    std::map<GLuint, std::pair<int, int>> sizes_;  // the textures made here: width, height
};

}  // namespace

std::unique_ptr<Display> make_gl_display(const char* title, int width, int height, SDL_WindowFlags flags, std::string& error) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow(title, width, height, flags | SDL_WINDOW_OPENGL);
    if (!window) {
        error = SDL_GetError();
        return nullptr;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        error = SDL_GetError();
        SDL_DestroyWindow(window);
        return nullptr;
    }
    SDL_GL_MakeCurrent(window, context);
    Api api;
    const char* missing = nullptr;
    GLint major = 0;
    GLint minor = 0;
    if (api.load(missing)) {
        api.GetIntegerv(MAJOR_VERSION, &major);
        api.GetIntegerv(MINOR_VERSION, &minor);
    }
    if (missing || major * 10 + minor < 33) {
        error = missing ? std::string("no ") + missing : "OpenGL " + std::to_string(major) + "." + std::to_string(minor) + " is below 3.3";
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        return nullptr;
    }
    return std::make_unique<GlDisplay>(window, context, api);
}

}  // namespace freetvc::sdl
