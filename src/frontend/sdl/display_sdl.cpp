// SPDX-License-Identifier: GPL-3.0-or-later
// The fallback display: SDL's renderer, no picture model.
#include "frontend/sdl/display_impl.hpp"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace freetvc::sdl {

namespace {

class SdlDisplay final : public Display {
public:
    SdlDisplay(SDL_Window* window, SDL_Renderer* renderer) : renderer_(renderer) {
        window_ = window;
        name_ = std::string("SDL renderer (") + SDL_GetRendererName(renderer) + ")";
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
    }
    ~SdlDisplay() override {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
    }

    std::string_view name() const override { return name_; }
    bool effects() const override { return false; }

    ImTextureID create_texture(int width, int height) override {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture) {
            return ImTextureID_Invalid;
        }
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
    }
    void update_texture(ImTextureID texture, const void* pixels, int pitch) override {
        SDL_UpdateTexture(as_sdl(texture), nullptr, pixels, pitch);
    }
    void destroy_texture(ImTextureID texture) override { SDL_DestroyTexture(as_sdl(texture)); }
    void set_vsync(bool on) override { SDL_SetRenderVSync(renderer_, on ? 1 : 0); }

    void new_frame() override {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
    }
    void present(Colour clear) override {
        SDL_SetRenderDrawColor(renderer_, clear.r, clear.g, clear.b, 0xFF);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

private:
    static SDL_Texture* as_sdl(ImTextureID texture) { return reinterpret_cast<SDL_Texture*>(static_cast<std::uintptr_t>(texture)); }

    SDL_Renderer* renderer_;
    std::string name_;
};

}  // namespace

std::unique_ptr<Display> make_sdl_display(const char* title, int width, int height, SDL_WindowFlags flags, std::string& error) {
    SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        error = SDL_GetError();
        if (window) {
            SDL_DestroyWindow(window);
        }
        return nullptr;
    }
    return std::make_unique<SdlDisplay>(window, renderer);
}

}  // namespace freetvc::sdl
