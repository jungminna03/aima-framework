#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace aima {

// Thin SDL3 window wrapper. Owns the SDL window and exposes both the SDL_Window*
// (what most renderers / SDL_GPU want) and the native Win32 HWND (what a DX12 /
// D3D swapchain wants). A project's Renderer::init takes the SDL_Window* and
// reaches for native_handle() only if its backend needs the raw OS handle.
//
// SDL_INIT_VIDEO and SDL_INIT_GAMEPAD are brought up here; audio is a separate
// subsystem (platform/audio.*). The framework owns no rendering — it only opens
// the window the project's Renderer then draws into.
class Window {
public:
    bool init(const std::string& title, uint32_t width, uint32_t height);
    void shutdown();

    SDL_Window* sdl() const { return window_; }
    // Win32 HWND on Windows; nullptr elsewhere (returned as void* so this header
    // pulls in no <windows.h>). Cast to HWND at the use site.
    void* native_handle() const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void set_size(uint32_t w, uint32_t h) { width_ = w; height_ = h; }

private:
    SDL_Window* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace aima
