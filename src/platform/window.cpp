#include "platform/window.h"

#include "core/log.h"

#include <SDL3/SDL.h>

namespace aima {

bool Window::init(const std::string& title, uint32_t width, uint32_t height) {
#if defined(__APPLE__)
    // macOS 14+ defaults SDL to NOT force a non-.app-bundle process to the
    // foreground (SDL_cocoaevents.m: background_app_default = true), so a binary
    // launched from a terminal shows its window but never becomes the KEY app —
    // keystrokes keep going to the terminal. Forcing this hint OFF re-enables SDL's
    // aggressive activation (dock-activate + activateIgnoringOtherApps in
    // applicationDidFinishLaunching) so the window actually receives keyboard input.
    // Must be set BEFORE the first video init / window creation (when finishLaunching
    // runs). Pairs with the SDL_RaiseWindow below.
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "0");
#endif

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        AIMA_ERROR("SDL_InitSubSystem failed: {}", SDL_GetError());
        return false;
    }

    // Headless capture runs (AIMA_SHOT: render N frames, screenshot, exit) keep
    // popping a real window over whatever the user is doing — create it HIDDEN
    // instead. D3D12/SDL_GPU render into a hidden window's swapchain just fine,
    // and the screenshot reads the backbuffer, so nothing else changes.
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char* shot = SDL_getenv("AIMA_SHOT");
    const char* rec  = SDL_getenv("AIMA_REC");   // 프레임 덤프 녹화 런도 화면을 안 뺏는다
    if ((shot && *shot) || (rec && *rec)) flags |= SDL_WINDOW_HIDDEN;

    window_ = SDL_CreateWindow(title.c_str(),
                               static_cast<int>(width),
                               static_cast<int>(height),
                               flags);
    if (!window_) {
        AIMA_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    if (flags & SDL_WINDOW_HIDDEN) AIMA_INFO("window hidden (AIMA_SHOT headless run)");

    width_ = width;
    height_ = height;
    AIMA_INFO("window created: {}x{}", width, height);

#if defined(__APPLE__)
    // Running the bare executable from a terminal (no .app bundle) leaves the
    // process as a background app on macOS 14+ — SDL won't force it to the
    // foreground (SDL_cocoaevents.m: background_app_default = true), so the window
    // never steals keyboard focus from the terminal and keystrokes go to the shell
    // instead of the game. Raise the window to pull the app foreground and make it
    // the key window (Cocoa_RaiseWindow does [NSApp activateIgnoringOtherApps:YES]
    // + makeKeyAndOrderFront).
    SDL_RaiseWindow(window_);
#endif

    return true;
}

void Window::shutdown() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

void* Window::native_handle() const {
    if (!window_) return nullptr;
    SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

} // namespace aima
