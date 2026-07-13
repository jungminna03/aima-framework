// ============================================================================
// sdlgpu/triangle_pass.cpp — Phase-2 rung 3: a hello triangle through SDL_GPU.
//
// PRAGMATIC SHADER PATH. The plan called for SDL_shadercross (runtime HLSL ->
// SPIR-V -> MSL), but its vcpkg port transitively requires `directx-dxc`, which
// has NO arm64-osx port ("directx-dxc is only supported on windows|linux-x64").
// So on macOS the shadercross route can't build. Since SDL_GPU's Metal backend
// accepts MSL source directly (SDL_GPU_SHADERFORMAT_MSL), we hand it MSL straight
// — no external shader compiler, no extra dependency, pixels on screen. (When a
// shadercross path becomes viable, HD2D_HAVE_SHADERCROSS gates swapping the HLSL
// source back in; the pipeline plumbing below is identical either way.)
//
// The triangle is generated entirely from the vertex id (no vertex buffer), so
// this proves device + shader compile-to-pipeline + draw + capture end to end.
// ============================================================================

#include "renderer/sdlgpu/triangle_pass.h"

#include "core/log_compat.h"

#include <SDL3/SDL.h>

namespace hd2d {

namespace {

// Metal shading language. A gradient-colored triangle generated from the vertex
// index. SDL_GPU's Metal backend feeds the vertex id in [[vertex_id]].
const char* kTriangleMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float4 color;
};

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    // Clip-space triangle (SDL_GPU is D3D/Metal-style: top-left origin, y up in clip).
    const float2 pos[3] = {
        float2( 0.0,  0.6),   // top
        float2(-0.6, -0.6),   // bottom-left
        float2( 0.6, -0.6),   // bottom-right
    };
    const float3 col[3] = {
        float3(1.0, 0.2, 0.2), // red
        float3(0.2, 1.0, 0.2), // green
        float3(0.2, 0.4, 1.0), // blue
    };
    VSOut o;
    o.position = float4(pos[vid], 0.0, 1.0);
    o.color = float4(col[vid], 1.0);
    return o;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
    return in.color;
}
)MSL";

SDL_GPUShader* make_shader(SDL_GPUDevice* gpu, const char* entry,
                           SDL_GPUShaderStage stage) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(kTriangleMSL);
    info.code_size = SDL_strlen(kTriangleMSL) + 1;  // include the NUL for MSL source
    info.entrypoint = entry;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    SDL_GPUShader* sh = SDL_CreateGPUShader(gpu, &info);
    if (!sh) HD2D_ERROR("triangle: SDL_CreateGPUShader({}) failed: {}", entry, SDL_GetError());
    return sh;
}

} // namespace

bool TrianglePass::init(SDL_GPUDevice* gpu, SDL_Window* window) {
    if (!gpu || !window) return false;

    // The Metal backend must accept MSL; if SDL chose a non-Metal driver (e.g.
    // Vulkan) this source won't compile and we bail (clear-only).
    if ((SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) == 0) {
        HD2D_WARN("triangle: device does not accept MSL shaders — skipping triangle");
        return false;
    }

    SDL_GPUShader* vs = make_shader(gpu, "vs_main", SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* fs = make_shader(gpu, "fs_main", SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(gpu, vs);
        if (fs) SDL_ReleaseGPUShader(gpu, fs);
        return false;
    }

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = SDL_GetGPUSwapchainTextureFormat(gpu, window);

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vs;
    pci.fragment_shader = fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pci.target_info.color_target_descriptions = &color_target;
    pci.target_info.num_color_targets = 1;
    // No vertex buffer: the vertices are baked into the vertex shader.

    pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);

    // Shaders are retained by the pipeline; release our references either way.
    SDL_ReleaseGPUShader(gpu, vs);
    SDL_ReleaseGPUShader(gpu, fs);

    if (!pipeline_) {
        HD2D_ERROR("triangle: SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        return false;
    }
    HD2D_INFO("triangle pass ready (MSL hello-triangle pipeline)");
    return true;
}

void TrianglePass::shutdown(SDL_GPUDevice* gpu) {
    if (pipeline_ && gpu) {
        SDL_ReleaseGPUGraphicsPipeline(gpu, pipeline_);
    }
    pipeline_ = nullptr;
}

void TrianglePass::draw(SDL_GPUCommandBuffer* /*cmd*/, SDL_GPURenderPass* pass,
                        uint32_t width, uint32_t height) {
    if (!pipeline_ || !pass) return;
    SDL_GPUViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.w = static_cast<float>(width);
    vp.h = static_cast<float>(height);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &vp);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

} // namespace hd2d
