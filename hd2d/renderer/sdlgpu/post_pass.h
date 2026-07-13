#pragma once

// ============================================================================
// sdlgpu/post_pass.h — HD-2D post chain on SDL_GPU (Phase-2 rung 2).
//
// The geometry pass renders the lit world into an HDR (RGBA16F) offscreen color
// target instead of straight to the swapchain. This pass then resolves it to
// the swapchain: a bloom pyramid (bright-pass + downsample/upsample blur) plus
// an AgX filmic tonemap — a direct MSL port of post_chain.cpp / post.hlsl.
//
// MSL inline shaders (SDL_shadercross is unavailable on arm64-osx). On the DX12
// backend this file is never compiled; post_chain.cpp stays the d3d12 path.
// ============================================================================

#include <cstdint>

extern "C" {
struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUGraphicsPipeline;
}

namespace hd2d {

// Tunables mirroring the DX12 PostSettings defaults.
struct PostParams {
    float exposure = 1.0f;
    float bloom_intensity = 0.06f;
    float bloom_threshold = 1.0f;
    float bloom_knee = 0.5f;
    bool  agx = true;          // AgX view transform vs plain sRGB encode
    bool  bloom = true;
};

class PostPass {
public:
    // Build the tonemap/bloom pipelines. swap_fmt is the swapchain format the
    // tonemap pipeline writes; hdr_fmt is the RGBA16F scene/bloom format.
    bool init(SDL_GPUDevice* gpu, SDL_Window* window);
    void shutdown(SDL_GPUDevice* gpu);

    bool valid() const { return tonemap_pipeline_ != nullptr; }

    // The HDR scene color target the geometry pass renders into. Reallocated on
    // resize. SAMPLER|COLOR_TARGET usage.
    SDL_GPUTexture* hdr_target(SDL_GPUDevice* gpu, uint32_t width, uint32_t height);

    // Resolve the HDR target to the swapchain: bloom pyramid + AgX tonemap.
    // Issues its own render passes on `cmd` (must be called outside any pass).
    void execute(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                 SDL_GPUTexture* swap_tex, uint32_t width, uint32_t height,
                 const PostParams& params);

    static constexpr uint32_t kBloomMips = 5;

private:
    bool ensure_bloom(SDL_GPUDevice* gpu, uint32_t width, uint32_t height);

    SDL_GPUGraphicsPipeline* tonemap_pipeline_ = nullptr;     // HDR -> swapchain
    SDL_GPUGraphicsPipeline* bloom_down_pipeline_ = nullptr;  // 13-tap downsample
    SDL_GPUGraphicsPipeline* bloom_up_pipeline_ = nullptr;    // 9-tap tent, additive
    SDL_GPUSampler*          linear_ = nullptr;               // linear-clamp

    SDL_GPUTexture* hdr_ = nullptr;
    uint32_t        hdr_w_ = 0;
    uint32_t        hdr_h_ = 0;

    SDL_GPUTexture* bloom_[kBloomMips]{};
    uint32_t        bloom_w_[kBloomMips]{};
    uint32_t        bloom_h_[kBloomMips]{};
    uint32_t        bloom_base_w_ = 0;
    uint32_t        bloom_base_h_ = 0;
};

} // namespace hd2d
