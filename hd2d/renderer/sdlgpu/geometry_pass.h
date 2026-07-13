#pragma once

// ============================================================================
// sdlgpu/geometry_pass.h — the real 3D world through SDL_GPU.
//
// Two modes:
//  (A) STANDALONE (rung 4, the original): loads the world .glb itself, uploads
//      pos+normal, and draws it lit by a fixed orbit camera framing the bounds.
//      Used as a fallback when the game hasn't handed over a live scene.
//  (B) LIVE (rung 1 & 2): renders a LiveScene the game's sdlgpu render system
//      fills from the ECS each frame — the ACTUAL map meshes at their live world
//      transforms, framed by the live OrbitCamera, plus camera-facing textured
//      billboard sprites. World-mesh GPU buffers are uploaded lazily and cached
//      by their CPU positions pointer; sprite textures are cached by path.
// ============================================================================

#include "core/math_compat.h"
#include "renderer/sdlgpu/live_scene.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

extern "C" {
struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
}

namespace hd2d {

class Dx12Device;

class GeometryPass {
public:
    // Build the GPU pipelines (world mesh + billboard) and load the standalone
    // fallback .glb. Non-fatal on failure (device degrades to triangle/clear).
    bool init(Dx12Device& dev, SDL_Window* window, const std::string& glb_path);
    void shutdown(SDL_GPUDevice* gpu);

    // valid() => the device has SOMETHING to draw (a live scene or the fallback).
    bool valid() const { return world_pipeline_ != nullptr; }

    // (A) Draw the standalone fallback world with a fixed framing camera.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
              uint32_t width, uint32_t height);

    // (B) Draw the live ECS scene (meshes + billboards) with its own camera.
    void draw_live(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                   SDL_GPURenderPass* pass, const LiveScene& scene,
                   uint32_t width, uint32_t height);

    // (B, shadows) Render the live meshes into the sun shadow map from the sun's
    // POV. The device calls this in its OWN depth-only render pass BEFORE the
    // color pass, so draw_live can sample it. Safe to call with no live scene.
    void render_shadow_map(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                           SDL_GPURenderPass* pass, const LiveScene& scene);
    // The sun shadow depth target the device opens its shadow pass against.
    SDL_GPUTexture* shadow_target(SDL_GPUDevice* gpu);
    static constexpr uint32_t kShadowSize = 2048;

    // The pass needs its OWN depth target; the device asks for it each frame.
    SDL_GPUTexture* depth_target(SDL_GPUDevice* gpu, uint32_t width, uint32_t height);

    Float3 bounds_min() const { return bmin_; }
    Float3 bounds_max() const { return bmax_; }

private:
    struct Node {                       // a standalone-fallback drawable
        SDL_GPUBuffer* vb = nullptr;
        SDL_GPUBuffer* ib = nullptr;
        uint32_t index_count = 0;
        dx::XMFLOAT4X4 model;
        float color[3] = {0.8f, 0.8f, 0.8f};
    };

    // A cached GPU mesh for the live path, keyed by the CPU positions pointer.
    struct CachedMesh {
        SDL_GPUBuffer* vb = nullptr;
        SDL_GPUBuffer* ib = nullptr;
        uint32_t index_count = 0;
    };
    const CachedMesh* mesh_for(SDL_GPUDevice* gpu, const LiveMesh& m);

    // A cached billboard sprite texture, keyed by its source path.
    struct CachedTexture {
        SDL_GPUTexture* tex = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        bool failed = false;            // decode/upload failed: never retry
    };
    const CachedTexture* texture_for(SDL_GPUDevice* gpu, const std::string& path);

    SDL_GPUGraphicsPipeline* world_pipeline_ = nullptr;       // lit pos+normal meshes
    SDL_GPUGraphicsPipeline* world_blend_pipeline_ = nullptr; // translucent occluder-fade variant
    SDL_GPUGraphicsPipeline* billboard_pipeline_ = nullptr;   // textured alpha-tested quads
    SDL_GPUGraphicsPipeline* shadow_pipeline_ = nullptr;      // depth-only sun caster
    SDL_GPUBuffer*           quad_vb_ = nullptr;              // shared billboard quad
    SDL_GPUBuffer*           quad_ib_ = nullptr;
    SDL_GPUSampler*          sampler_ = nullptr;              // nearest-clamp (pixel art)
    SDL_GPUSampler*          shadow_sampler_ = nullptr;       // linear-clamp PCF compare

    SDL_GPUTexture* shadow_tex_ = nullptr;                    // sun depth map (kShadowSize^2)

    std::vector<Node> nodes_;                                // standalone fallback
    std::map<const void*, CachedMesh> mesh_cache_;           // key = positions->data()
    int seen_map_generation_ = 0;   // LiveScene.map_generation 마지막 관측치
    void flush_mesh_cache(SDL_GPUDevice* gpu);   // 맵 스왑: 포인터-키 캐시 무효화
    std::map<std::string, CachedTexture> tex_cache_;

    SDL_GPUTexture* depth_ = nullptr;
    uint32_t depth_w_ = 0;
    uint32_t depth_h_ = 0;

    Float3 bmin_{-10, -1, -10};
    Float3 bmax_{10, 8, 10};
};

} // namespace hd2d
