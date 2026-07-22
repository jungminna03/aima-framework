#include "platform/window.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <cstdlib>   // std::getenv (headless / test-mode detection)

namespace aima {

bool Window::init(const std::string& title, uint32_t width, uint32_t height) {
    // Headless test/screenshot mode: the behavioural suite launches this exe ~100 times with
    // AIMA_MAXFRAMES (frame cap) / AIMA_SHOT (screenshot) — and AIMA_REC frame-dump capture
    // runs (upstream 0f76163) shouldn't take over the screen either. Creating a VISIBLE window
    // each time flashes a game window on screen over and over. So under those env vars, create
    // the window HIDDEN and skip show/raise — rendering, screenshots, and the coord math all
    // work hidden. The coord/focus platform tests set RC_REQUIRE_FOCUS / RC_WARP_PROBE to keep
    // a real window.
    const bool needsRealWindow = std::getenv("RC_REQUIRE_FOCUS") != nullptr;
    const bool headless = (std::getenv("AIMA_MAXFRAMES") || std::getenv("AIMA_SHOT") ||
                           std::getenv("AIMA_REC")) && !needsRealWindow;

    // Let SDL raise + focus a freshly created window even if another app (the launching
    // Terminal / IDE) currently holds the foreground. Without this the window opens
    // INACTIVE on macOS — the title bar text renders GRAY and it never becomes the key
    // window, so keyboard/mouse focus never lands on it.
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        AIMA_ERROR("SDL_InitSubSystem failed: {}", SDL_GetError());
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (headless) flags |= SDL_WINDOW_HIDDEN;   // don't flash a window during tests/screenshots
    window_ = SDL_CreateWindow(title.c_str(),
                               static_cast<int>(width),
                               static_cast<int>(height),
                               flags);
    if (!window_) {
        AIMA_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    if (flags & SDL_WINDOW_HIDDEN) AIMA_INFO("window hidden (AIMA_SHOT headless run)");

    // Make the window the active/key window NOW: show it, then raise it (raise also
    // requests input focus). On macOS this flips the title bar from gray (inactive) to
    // black (active) and routes keyboard/mouse to the game. SDL_FlashWindow nudges the
    // OS to surface it if the launching app was frontmost. Skipped when headless (hidden).
    if (!headless) {
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
    }

    width_ = width;
    height_ = height;
    const bool focused = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
    AIMA_INFO("window created: {}x{} focus={}", width, height, focused ? 1 : 0);
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
