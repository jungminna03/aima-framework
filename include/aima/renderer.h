#pragma once

// ============================================================================
// aima/renderer.h — the abstract Renderer interface.
//
// THIS IS THE LOAD-BEARING SEAM of the renderer-LESS framework. The framework
// owns NO concrete renderer: it opens an SDL window, runs the host loop, and
// drives the ECS — but every pixel is drawn by an implementation a PROJECT
// supplies and registers. AIMA's HD-2D game implements this with a 3D backend
// (DX12 / SDL_GPU-Metal); a 2D Siv3D game implements it with Siv3D draw calls;
// the minimal example implements it with a bare SDL clear-to-color. None of that
// graphics code lives here.
//
// The host calls exactly these methods, in this lifecycle:
//
//   startup:        renderer->init(window, w, h)        // once, after the window opens
//   per frame:      void* frame = renderer->begin_frame(clear_color);
//                   //  ... the ECS Render-phase systems draw into `frame` ...
//                   renderer->end_frame();              // submit + present
//   on resize:      renderer->resize(w, h)              // from the window event
//   on request:     renderer->screenshot(path)          // headless verification, F12, etc.
//   shutdown:       renderer->shutdown()                // once, before the window closes
//
// begin_frame returns an OPAQUE frame handle (void*). The framework never
// dereferences it — it only passes it to the game's render systems (e.g. via a
// World resource the project defines). The handle's real type is the project's
// own (a command list, an SDL_GPURenderPass*, nothing at all). A backend with no
// per-frame object simply returns a non-null sentinel (or nullptr — the project's
// own render systems decide what nullptr means to them).
//
// Threading: every method is called on the host (main) thread, between SDL event
// pumping and the next frame. Implementations need no internal locking for the
// host's calls.
// ============================================================================

#include <cstdint>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace aima {

// RGBA, linear or sRGB per the backend's swapchain convention; [0,1].
struct ClearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// Opaque per-frame handle. Defined as void* so the framework stays renderer-less;
// the project casts it back to whatever its begin_frame produced.
using FrameHandle = void*;

class Renderer {
public:
    virtual ~Renderer() = default;

    // Create the backend's device + swapchain for `window` at `width`x`height`.
    // `window` is the SDL_Window the framework opened (Window::sdl()); a backend
    // that needs the raw OS handle (a DX12 swapchain wants the Win32 HWND) can get
    // it from Window::native_handle() — the host passes the Window through so the
    // project chooses. Return false to abort startup (the host tears down + exits).
    virtual bool init(SDL_Window* window, int width, int height) = 0;

    // Begin a frame: acquire the swapchain image and start a render pass that
    // clears to `clear`. Return an opaque handle the game's Render-phase systems
    // record their draws against (or a sentinel / nullptr if the backend has no
    // such object). Called once per frame, before App::Tick's Render phase.
    // Forward one SDL event to the backend's UI layer (Dear ImGui). The host
    // calls this for EVERY polled event, before its own handling — 2026-07-15:
    // 이 배선이 aima 마이그레이션 때 빠져 ImGui가 마우스 이벤트를 못 받아
    // "F1 패널이 클릭이 안 되는" 버그의 근원이었다. Default no-op.
    virtual void process_event(const SDL_Event& /*event*/) {}

    // True while the backend's UI layer (ImGui) is using the mouse — the host
    // skips filling game mouse input so panel clicks don't leak into gameplay
    // (구 main.cpp의 io.WantCaptureMouse 게이팅 복원, 2026-07-15).
    virtual bool wants_mouse() const { return false; }

    virtual FrameHandle begin_frame(const ClearColor& clear) = 0;

    // End the frame: finish the render pass, submit, and present. Called once per
    // frame, after App::Tick. `vsync` is a hint (ignore if the backend can't honor
    // it). After this returns the FrameHandle from begin_frame is invalid.
    virtual void end_frame(bool vsync = true) = 0;

    // Swapchain resize (from SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED). Safe to call
    // outside begin/end_frame; the host calls it between frames.
    virtual void resize(int width, int height) = 0;

    // Capture the most recently presented frame to `path` (PNG recommended). Used
    // by the headless verification harness (AIMA_SHOT) and any in-game screenshot
    // key. Backends that can't read back the swapchain may no-op + warn. Default:
    // no-op, so a minimal renderer needn't implement it.
    virtual void screenshot(const std::string& /*path*/) {}

    // Block until the GPU is idle. The host calls this before teardown/resize so
    // no in-flight frame references resources about to be freed. Default: no-op
    // for backends that present synchronously.
    virtual void flush() {}

    // Destroy the device + swapchain. Called once at shutdown, before the window
    // is destroyed. After this the Renderer must not be used again.
    virtual void shutdown() = 0;
};

} // namespace aima
