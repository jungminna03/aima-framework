#pragma once

// ============================================================================
// sdlgpu/device.h — REAL SDL_GPU device for the macOS/Linux backend (Phase 2).
//
// Keeps the EXACT class name (`hd2d::Dx12Device`) and the public method surface
// the host (main.cpp) and the game/renderer call, so zero game/host call sites
// change shape. Internally this is now a genuine SDL3 GPU device (Metal on
// macOS): per-frame command buffer + swapchain texture + a render pass that
// clears, real framebuffer readback for screenshots, and (Phase-2 rung 3) a
// hello-triangle drawn through a runtime-cross-compiled HLSL pipeline.
//
// begin_frame() still returns nullptr: the game's render systems are written
// against raw ID3D12GraphicsCommandList* and early-return on `!rc->gpu_cmd`, so
// the DX12-coupled scene path stays inert on this backend. The SDL_GPU drawing
// this device does (clear + triangle) happens entirely inside begin/end_frame.
//
// On Windows (HD2D_RENDERER_D3D12) the real renderer/dx12/device.h is used and
// this file is never compiled.
// ============================================================================

#include "renderer/sdlgpu/d3d12_stubs.h"
#include "renderer/sdlgpu/live_scene.h"

#include <cstdint>
#include <string>

// Opaque SDL forward declarations (no SDL headers leak through this header).
extern "C" {
struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUGraphicsPipeline;
}

namespace hd2d {

class TrianglePass;  // sdlgpu/triangle_pass.h — owns the hello-triangle pipeline
class GeometryPass;  // sdlgpu/geometry_pass.h — owns the world-mesh pipeline
class PostPass;      // sdlgpu/post_pass.h — owns the bloom + tonemap chain

class Dx12Device {
public:
    // Mirror the real device's public format/frame constants so the renderer
    // headers (forward_pass.h ring sizing, scene_targets, imgui_layer) compile.
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr uint32_t kFrameCount = 3;

    // window_handle is an SDL_Window* (Window::sdl()) on this backend — NOT the
    // Win32 HWND the DX12 device takes. The host passes the right one per backend.
    bool init(void* window_handle, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    // Acquire a command buffer + swapchain texture and open a render pass that
    // clears to `clear_color`. Returns nullptr (no DX12 command list): the game
    // render systems no-op; the SDL_GPU clear/triangle is done here.
    ID3D12GraphicsCommandList* begin_frame(const float clear_color[4]);
    // Ends the render pass, optionally reads back the swapchain for a screenshot,
    // and submits the command buffer.
    void end_frame(bool vsync);

    void request_screenshot(const std::string& path) { capture_path_ = path; }

    // --- Live game scene bridge (sdlgpu rung 1 & 2) ---
    // The game's sdlgpu render system fills a LiveScene from the ECS each frame
    // and hands it here BEFORE the host calls begin_frame; begin_frame renders
    // it (live world + live camera + billboards) instead of geometry_pass's own
    // glb load. An invalid/empty scene falls back to that independent glb.
    LiveScene& live_scene() { return live_; }
    void set_live_scene_valid(bool v) { live_.valid = v; }

    void flush(); // waits for GPU idle (teardown/resize barrier)

    // --- accessors the renderer/imgui code reads; all return null/zero stubs ---
    ID3D12Device*         device()    const { return nullptr; }
    ID3D12CommandQueue*   queue()     const { return nullptr; }
    ID3D12DescriptorHeap* srv_heap()  const { return nullptr; }
    uint32_t              srv_increment() const { return 0; }
    uint32_t              width()     const { return width_; }
    uint32_t              height()    const { return height_; }

    D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv() const { return {}; }

    // Descriptor allocation: hand out distinct dummy handles so callers that
    // check `srv_cpu.ptr` see a non-zero value. Never touches a real heap.
    bool alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu);
    void free_srv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

    // --- SDL_GPU accessors (Phase 2). Null on the DX12 backend (file not built). ---
    SDL_GPUDevice* gpu() const { return gpu_; }
    // The current frame's command buffer + swapchain texture, valid only between
    // begin_frame and end_frame (both null otherwise / when the window can't
    // present this frame). The ImGui layer records the UI onto the swapchain via
    // these, after the scene resolve but before the device submits.
    SDL_GPUCommandBuffer* gpu_cmd()       const { return cmd_; }
    SDL_GPUTexture*       swap_texture()  const { return swap_tex_; }

private:
    void write_screenshot(const void* rgba, uint32_t w, uint32_t h);

    SDL_Window*          window_ = nullptr;   // not owned (Window owns it)
    SDL_GPUDevice*       gpu_ = nullptr;       // owned (created in init)

    // Per-frame transient handles, valid only between begin_frame/end_frame.
    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPUTexture*       swap_tex_ = nullptr;
    uint32_t              swap_w_ = 0;
    uint32_t              swap_h_ = 0;

    TrianglePass*        triangle_ = nullptr;  // owned (created in init)
    GeometryPass*        geometry_ = nullptr;  // owned (created in init; may be null)
    PostPass*            post_ = nullptr;       // owned (created in init; may be null)

    LiveScene     live_;                 // filled by the game's sdlgpu render system

    std::string   capture_path_;
    uint32_t      width_ = 0;
    uint32_t      height_ = 0;
    uint64_t      next_srv_ = 1;         // monotonic dummy descriptor id
};

} // namespace hd2d
