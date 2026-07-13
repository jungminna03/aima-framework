#pragma once

// ============================================================================
// sdlgpu/triangle_pass.h — the Phase-2 "hello triangle" through SDL_GPU.
//
// Compiles a trivial HLSL vertex+fragment shader at RUNTIME via SDL_shadercross
// (HLSL -> SPIR-V -> MSL/SPIR-V for the live driver), builds an SDL_GPU graphics
// pipeline, and draws one triangle into whatever render pass the device opened.
// This proves the full pipeline: device + runtime shader cross-compile + PSO +
// draw + (the device's) framebuffer capture.
//
// init() returning false is non-fatal at the device level (the device degrades
// to clear-only), so the engine still boots if shadercross is unavailable.
// ============================================================================

extern "C" {
struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUGraphicsPipeline;
}

#include <cstdint>

namespace hd2d {

class TrianglePass {
public:
    bool init(SDL_GPUDevice* gpu, SDL_Window* window);
    void shutdown(SDL_GPUDevice* gpu);

    // Record the triangle draw into an already-open render pass. width/height are
    // the swapchain dimensions (for the viewport).
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
              uint32_t width, uint32_t height);

private:
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
};

} // namespace hd2d
