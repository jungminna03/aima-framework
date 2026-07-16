#pragma once

// ============================================================================
// renderer/renderer_impl.h — Hd2dRenderer : public aima::Renderer
//
// The single load-bearing seam between the renderer-LESS aima framework and
// HD2D's concrete 3D backend. The framework (aima::Host) owns no renderer; it
// drives whatever aima::Renderer the project hands to Host::run(). This class
// IS that object for HD2D: it OWNS/composes the existing hd2d::Dx12Device
// (real DX12 on Windows / SDL_GPU clear+scene stub on macOS — same class name,
// same public surface, chosen at build time by HD2D_RENDERER_D3D12) and adapts
// HD2D's device lifecycle onto aima::Renderer's six virtuals.
//
//   aima lifecycle              ->  Hd2dRenderer delegates to hd2d::Dx12Device
//   ---------------------------------------------------------------------------
//   init(SDL_Window*, w, h)     ->  device_.init(<native handle>, w, h)
//   begin_frame(ClearColor)     ->  (FrameHandle)device_.begin_frame(cc[4])
//   end_frame(vsync)            ->  device_.end_frame(vsync)
//   resize(w, h)                ->  device_.resize(w, h)
//   screenshot(path)            ->  device_.request_screenshot(path)  (see note)
//   flush()                     ->  device_.flush()
//   shutdown()                  ->  device_.shutdown()
//
// IMPORTANT — what this class does NOT do:
//   * It does NOT modify hd2d::Dx12Device, the passes, or any renderer internal.
//   * It does NOT own the passes (ForwardPass/ShadowPass/PostChain/SceneTargets)
//     or the ImGui layer. Those are renderer SYSTEMS the GAME module owns and
//     drives during App::Tick's Render phase. They reach the GPU through this
//     renderer's PUBLIC device() accessor (and the DX12 sub-accessors below),
//     exactly as they did when main.cpp owned a bare hd2d::Dx12Device.
//
// The DX12-specific accessors (device()/queue()/srv_heap()/...) are kept PUBLIC
// and forward 1:1 to the owned device so game render systems keep compiling and
// linking against the same surface. On the SDL_GPU backend those forward to the
// stub device's null-returning accessors (identical to today's behaviour).
// ============================================================================

#include <string>

#include "aima/renderer.h"          // aima::Renderer, aima::ClearColor, aima::FrameHandle
#include "renderer/device.h"        // hd2d::Dx12Device (DX12 real | SDL_GPU stub)
#include "renderer/gpu_resources.h" // hd2d::Dx12ResourceTable (rhi 핸들 테이블, host 소유)
#include "ui/imgui_layer.h"         // hd2d::ImGuiLayer (owns the Dear ImGui context)

// Forward-declared DX12 handle types so this header pulls in no <d3d12.h> on
// callers that only need the renderer lifecycle. On the SDL_GPU backend these
// names resolve to the d3d12_stubs.h placeholders (already included via
// renderer/device.h -> sdlgpu/device.h), so the signatures stay identical.
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;

namespace hd2d {

// aima::Renderer implementation backed by HD2D's hd2d::Dx12Device.
class Hd2dRenderer final : public aima::Renderer {
public:
    Hd2dRenderer() = default;
    ~Hd2dRenderer() override = default;

    Hd2dRenderer(const Hd2dRenderer&)            = delete;
    Hd2dRenderer& operator=(const Hd2dRenderer&) = delete;

    // ---- aima::Renderer (the six host-driven virtuals) ----------------------

    // Windows: extract the Win32 HWND from the SDL_Window's properties and call
    // device_.init(hwnd, w, h). macOS/Linux (SDL_GPU): pass the SDL_Window*
    // straight through (the stub device's init takes a void* window handle).
    // Returns false to abort host startup.
    bool init(SDL_Window* window, int width, int height) override;

    // float cc[4] = {r,g,b,a}; returns (FrameHandle)device_.begin_frame(cc).
    // On DX12 that is the open ID3D12GraphicsCommandList* cast to void*; on the
    // SDL_GPU backend it is nullptr (device_.begin_frame returns nullptr there,
    // and the game's DX12 render systems early-return on a null command list).
    aima::FrameHandle begin_frame(const aima::ClearColor& clear) override;

    // 호스트가 폴링한 SDL 이벤트를 ImGui 백엔드로 전달(2026-07-15 — 빠져 있던 배선:
    // 이게 없으면 ImGui가 마우스 위치/버튼을 못 받아 디버그 패널 클릭 불능).
    void process_event(const SDL_Event& event) override;
    bool wants_mouse() const override;   // ImGui io.WantCaptureMouse(패널 클릭이 게임에 안 새게)

    void end_frame(bool vsync = true) override;
    void resize(int width, int height) override;

    // Adapts HD2D's request_screenshot()+end_frame consume model to aima's
    // "capture to path now" call: queues `path` on the device so the NEXT
    // end_frame() copies the presented frame to a readback buffer and writes the
    // PNG. See renderer_impl.cpp for the exact semantics + the immediate-capture
    // helper (capture_now) the game's F12/debug path can use instead.
    void screenshot(const std::string& path) override;

    void flush() override;
    void shutdown() override;

    // ---- HD2D backend access for the game's render systems ------------------
    //
    // The owned device, by reference. The game's Gfx resource and the renderer
    // passes (ForwardPass/ShadowPass/PostChain/SceneTargets) take an
    // hd2d::Dx12Device& in their init()/render() calls — they get it from here.
    hd2d::Dx12Device&       device_obj()       { return device_; }
    const hd2d::Dx12Device& device_obj() const { return device_; }

    // Host-owned rhi handle table (PAL render keystone). Lives HERE (not the game)
    // so it survives game-module hot-reload; populated at asset load and resolved
    // by the renderer when drawing the LiveScene packet (later bricks).
    Dx12ResourceTable&       resource_table()       { return resource_table_; }
    const Dx12ResourceTable& resource_table() const { return resource_table_; }

    // The current frame's open DX12 command list (valid between begin_frame and
    // end_frame); nullptr on SDL_GPU. The host wires this into the game's
    // hd2d::RenderContext.gpu_cmd each frame (scene_hook, right after begin_frame)
    // so the DX12 render systems can record. This is the FrameHandle the migration
    // notes refer to — without it every 3D pass early-returns on a null cmd list.
    ID3D12GraphicsCommandList* current_command_list() const { return cur_cmd_; }

    // DX12-specific sub-accessors, forwarded 1:1 to the owned device so existing
    // render systems / imgui glue keep their call sites. Null on the SDL_GPU
    // backend (the stub device returns null/zero — unchanged behaviour).
    ID3D12Device*         device()        const { return device_.device(); }
    ID3D12CommandQueue*   queue()         const { return device_.queue(); }
    ID3D12DescriptorHeap* srv_heap()      const { return device_.srv_heap(); }
    uint32_t              srv_increment() const { return device_.srv_increment(); }
    uint32_t              width()         const { return device_.width(); }
    uint32_t              height()        const { return device_.height(); }

    // Immediate-capture convenience: request + end the current frame is NOT done
    // here (the host owns the frame loop); this only queues the path, same as
    // screenshot(). Provided so game-side debug code has an intent-named entry.
    void request_screenshot(const std::string& path) { device_.request_screenshot(path); }

    // ---- ImGui context handoff to the game module ---------------------------
    //
    // ImGui is statically linked into hd2d_renderer, which links into BOTH the exe
    // and the hd2d_game module — so each binary carries its OWN ImGui globals. The
    // renderer (this class) owns the single live context via imgui_; the game
    // module's GameBindHost adopts it. This exposes that context + the allocator
    // functions as opaque pointers (no <imgui.h> in this header) so the host's
    // bind-provider can marshal them across the ABI. Defined in renderer_impl.cpp.
    void imgui_bind_args(void*& ctx, void*& alloc, void*& free, void*& user) const;

private:
    // The composed backend device. Owns the GPU, swapchain, fences. Same class
    // name on every backend; the concrete type is selected by HD2D_RENDERER_D3D12.
    hd2d::Dx12Device device_{};

    // rhi 핸들 ↔ DX12 리소스 매핑(host 소유 → 게임 핫스왑에도 생존). brick2의
    // Dx12ResourceTable. 아직 채우거나 읽는 곳 없음(다음 brick에서 배선).
    Dx12ResourceTable resource_table_{};

    // Owns the Dear ImGui context + its SDL3/(DX12) backends. Faithful to the old
    // main.cpp which owned an hd2d::ImGuiLayer; the renderer now owns it so the
    // context exists by the time the host binds it into the game module.
    hd2d::ImGuiLayer imgui_{};

    // The current frame's command list (DX12) / nullptr (SDL_GPU), captured in
    // begin_frame so end_frame can hand it to imgui_.end_frame for ImGui draws.
    ID3D12GraphicsCommandList* cur_cmd_ = nullptr;

    bool initialized_ = false;
};

} // namespace hd2d
