// ============================================================================
// sdlgpu/geometry_pass.cpp — the real 3D world on SDL_GPU.
//
// MSL-shader path (SDL_shadercross can't build on arm64-osx; the Metal backend
// takes MSL directly). Provides two render modes:
//   (A) STANDALONE fallback: loads the world .glb, derives smooth normals,
//       uploads pos+normal, draws every node lit by a fixed framing camera.
//   (B) LIVE: renders the LiveScene the game's sdlgpu render system fills from
//       the ECS — the actual map meshes at their live transforms, framed by the
//       live OrbitCamera, plus camera-facing textured billboard sprites. Mesh
//       GPU buffers upload lazily (cached by CPU positions pointer); sprite
//       textures load+upload lazily (cached by path).
// ============================================================================

#include "renderer/sdlgpu/geometry_pass.h"

#include "assets/gltf_loader.h"
#include "core/log_compat.h"
#include "renderer/camera.h"
#include "renderer/device.h"

#include <SDL3/SDL.h>

#include <stb_image.h>

#include <cmath>

namespace hd2d {

namespace {

struct PosNormal {
    float px, py, pz;
    float nx, ny, nz;
};

// Per-frame world-mesh uniform: model*viewProj + model (for the world normal),
// a directional light, and the node base color. Row-major (engine convention);
// the MSL multiplies row-vector * matrix to match.
struct VSUniform {
    dx::XMFLOAT4X4 mvp;
    dx::XMFLOAT4X4 model;
    dx::XMFLOAT4X4 sun_view_proj;  // light-space transform (shadow map sample)
    float light_dir[4];   // xyz world-space direction TO the light
    float color[4];       // node base color; w = ambient packing unused
    float ambient[4];     // world ambient (xyz)
    float shadow[4];      // x texel, y depth bias, z normal-offset, w active(>0)
};

// Depth-only shadow caster uniform: model * sun_view_proj.
struct ShadowUniform {
    dx::XMFLOAT4X4 mvp;   // model * sun_view_proj
};

// Per-frame billboard uniform: the quad's model*viewProj, a uv transform that
// selects the animation frame + facing row, and a color tint.
struct BBUniform {
    dx::XMFLOAT4X4 mvp;
    float uv_off_scale[4]; // (off.x, off.y, scale.x, scale.y)
    float tint[4];         // rgb tint, a unused
};

// ---- world-mesh shader (lit, depth). Both stages in one source so the layouts
//      stay in lockstep; the fragment reads light_dir from the same uniform. ---
const char* kGeometryMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSUniform {
    float4x4 mvp;
    float4x4 model;
    float4x4 sun_view_proj;
    float4   light_dir;
    float4   color;
    float4   ambient;
    float4   shadow;   // x texel, y bias, z normal-offset, w active
};

struct VSIn {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct VSOut {
    float4 position [[position]];
    float3 world_pos;
    float3 normal;
    float3 color;
    float3 ambient;
};

static float4 mul_row(float4 v, float4x4 m) {
    return float4(dot(v, float4(m[0][0], m[1][0], m[2][0], m[3][0])),
                  dot(v, float4(m[0][1], m[1][1], m[2][1], m[3][1])),
                  dot(v, float4(m[0][2], m[1][2], m[2][2], m[3][2])),
                  dot(v, float4(m[0][3], m[1][3], m[2][3], m[3][3])));
}

vertex VSOut vs_main(VSIn in [[stage_in]], constant VSUniform& u [[buffer(0)]]) {
    VSOut o;
    o.position = mul_row(float4(in.pos, 1.0), u.mvp);
    o.world_pos = mul_row(float4(in.pos, 1.0), u.model).xyz;
    o.normal = mul_row(float4(in.normal, 0.0), u.model).xyz;
    o.color = u.color.xyz;
    o.ambient = u.ambient.xyz;
    return o;
}

// PCF 3x3 sun shadow factor (1 = fully lit). Mirrors standard.hlsl's
// shadow_factor: normal-offset + depth bias fight acne on the low-poly slopes.
// Metal clip-space z is already [0,1] (like D3D), so no remap; the light-space
// xy maps to UV with the standard (0.5, -0.5) + 0.5 (top-left origin).
static float shadow_factor(constant VSUniform& u, float3 wp, float3 N,
                           depth2d<float> shadow_map, sampler shadow_smp) {
    if (u.shadow.w < 0.5) return 1.0;
    float3 p = wp + N * u.shadow.z;
    float4 sc = mul_row(float4(p, 1.0), u.sun_view_proj);
    if (sc.w <= 0.0) return 1.0;
    float3 ndc = sc.xyz / sc.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float ref = ndc.z - u.shadow.y;
    float sum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            sum += shadow_map.sample_compare(
                shadow_smp, uv + float2(dx, dy) * u.shadow.x, ref);
    return sum / 9.0;
}

fragment float4 fs_main(VSOut in [[stage_in]], constant VSUniform& u [[buffer(0)]],
                        depth2d<float> shadow_map [[texture(0)]],
                        sampler shadow_smp [[sampler(0)]]) {
    float3 N = normalize(in.normal);
    float3 L = normalize(u.light_dir.xyz);
    float ndl = max(dot(N, L), 0.0);
    float s = shadow_factor(u, in.world_pos, N, shadow_map, shadow_smp);
    float3 lit = in.color * (in.ambient + s * ndl * float3(1.0, 0.96, 0.88));
    return float4(lit, u.color.w);   // w = occluder-fade alpha (1 = opaque)
}
)MSL";

// ---- depth-only shadow caster (sun POV). No color target; writes depth only. -
const char* kShadowMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ShadowUniform { float4x4 mvp; };

struct VSIn {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

static float4 mul_row(float4 v, float4x4 m) {
    return float4(dot(v, float4(m[0][0], m[1][0], m[2][0], m[3][0])),
                  dot(v, float4(m[0][1], m[1][1], m[2][1], m[3][1])),
                  dot(v, float4(m[0][2], m[1][2], m[2][2], m[3][2])),
                  dot(v, float4(m[0][3], m[1][3], m[2][3], m[3][3])));
}

vertex float4 sh_vs(VSIn in [[stage_in]], constant ShadowUniform& u [[buffer(0)]]) {
    return mul_row(float4(in.pos, 1.0), u.mvp);
}

// SDL_GPU requires a non-null fragment shader even for a 0-color depth pass.
// This writes nothing (no color target); only depth is recorded.
fragment void sh_fs() {}
)MSL";

// ---- billboard shader (textured, alpha-tested) -----------------------------
const char* kBillboardMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BBUniform {
    float4x4 mvp;
    float4   uv_off_scale;  // off.xy, scale.zw
    float4   tint;
};

struct VSIn {
    float3 pos [[attribute(0)]];
    float2 uv  [[attribute(1)]];
};

struct VSOut {
    float4 position [[position]];
    float2 uv;
    float3 tint;
};

static float4 mul_row(float4 v, float4x4 m) {
    return float4(dot(v, float4(m[0][0], m[1][0], m[2][0], m[3][0])),
                  dot(v, float4(m[0][1], m[1][1], m[2][1], m[3][1])),
                  dot(v, float4(m[0][2], m[1][2], m[2][2], m[3][2])),
                  dot(v, float4(m[0][3], m[1][3], m[2][3], m[3][3])));
}

vertex VSOut bb_vs(VSIn in [[stage_in]], constant BBUniform& u [[buffer(0)]]) {
    VSOut o;
    o.position = mul_row(float4(in.pos, 1.0), u.mvp);
    o.uv = in.uv * u.uv_off_scale.zw + u.uv_off_scale.xy;
    o.tint = u.tint.xyz;
    return o;
}

fragment float4 bb_fs(VSOut in [[stage_in]],
                      texture2d<float> tex [[texture(0)]],
                      sampler smp [[sampler(0)]]) {
    float4 c = tex.sample(smp, in.uv);
    if (c.a < 0.5) discard_fragment();   // pixel-art cutout
    return float4(c.rgb * in.tint, 1.0);
}
)MSL";

SDL_GPUShader* make_shader(SDL_GPUDevice* gpu, const char* code, const char* entry,
                           SDL_GPUShaderStage stage, uint32_t num_uniforms,
                           uint32_t num_samplers) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(code);
    info.code_size = SDL_strlen(code) + 1;
    info.entrypoint = entry;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_uniform_buffers = num_uniforms;
    info.num_samplers = num_samplers;
    SDL_GPUShader* sh = SDL_CreateGPUShader(gpu, &info);
    if (!sh) HD2D_ERROR("geometry: SDL_CreateGPUShader({}) failed: {}", entry, SDL_GetError());
    return sh;
}

// Upload a host buffer to a fresh GPU buffer of the given usage.
SDL_GPUBuffer* upload_buffer(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                             const void* data, uint32_t size, SDL_GPUBufferUsageFlags usage) {
    SDL_GPUBufferCreateInfo bci{};
    bci.usage = usage;
    bci.size = size;
    SDL_GPUBuffer* buf = SDL_CreateGPUBuffer(gpu, &bci);
    if (!buf) return nullptr;

    SDL_GPUTransferBufferCreateInfo tci{};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = size;
    SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(gpu, &tci);
    if (!xfer) { SDL_ReleaseGPUBuffer(gpu, buf); return nullptr; }

    void* mapped = SDL_MapGPUTransferBuffer(gpu, xfer, false);
    SDL_memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(gpu, xfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{};
    src.transfer_buffer = xfer;
    SDL_GPUBufferRegion dst{};
    dst.buffer = buf;
    dst.size = size;
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);

    SDL_ReleaseGPUTransferBuffer(gpu, xfer);
    return buf;
}

// Build smooth per-vertex normals (engine space) from a positions + index list.
void smooth_normals(const std::vector<Float3>& pos, const std::vector<uint32_t>& idx,
                    std::vector<PosNormal>& out) {
    out.resize(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
        out[i] = PosNormal{pos[i].x, pos[i].y, pos[i].z, 0, 0, 0};
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= pos.size() || b >= pos.size() || c >= pos.size()) continue;
        const Float3& pa = pos[a];
        const Float3& pb = pos[b];
        const Float3& pc = pos[c];
        const float ux = pb.x - pa.x, uy = pb.y - pa.y, uz = pb.z - pa.z;
        const float vx = pc.x - pa.x, vy = pc.y - pa.y, vz = pc.z - pa.z;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        for (uint32_t vi : {a, b, c}) {
            out[vi].nx += nx; out[vi].ny += ny; out[vi].nz += nz;
        }
    }
    for (PosNormal& v : out) {
        const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (len > 1e-8f) { v.nx /= len; v.ny /= len; v.nz /= len; }
        else { v.ny = 1.0f; }
    }
}

} // namespace

bool GeometryPass::init(Dx12Device& dev, SDL_Window* window, const std::string& glb_path) {
    SDL_GPUDevice* gpu = dev.gpu();
    if (!gpu || !window) return false;
    if ((SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) == 0) return false;

    // The lit world + billboards render into the RGBA16F HDR scene target (the
    // post pass tonemaps it to the swapchain), so the geometry pipelines' color
    // target format is the HDR format, NOT the swapchain format.
    const SDL_GPUTextureFormat swap_fmt = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    (void)window;

    // ---- world-mesh pipeline ----
    {
        SDL_GPUShader* vs = make_shader(gpu, kGeometryMSL, "vs_main",
                                        SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        SDL_GPUShader* fs = make_shader(gpu, kGeometryMSL, "fs_main",
                                        SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        if (!vs || !fs) {
            if (vs) SDL_ReleaseGPUShader(gpu, vs);
            if (fs) SDL_ReleaseGPUShader(gpu, fs);
            return false;
        }

        SDL_GPUVertexBufferDescription vbd{};
        vbd.slot = 0;
        vbd.pitch = sizeof(PosNormal);
        vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2]{};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[1].offset = sizeof(float) * 3;

        SDL_GPUColorTargetDescription color_target{};
        color_target.format = swap_fmt;

        SDL_GPUGraphicsPipelineCreateInfo pci{};
        pci.vertex_shader = vs;
        pci.fragment_shader = fs;
        pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
        pci.vertex_input_state.num_vertex_buffers = 1;
        pci.vertex_input_state.vertex_attributes = attrs;
        pci.vertex_input_state.num_vertex_attributes = 2;
        pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        pci.depth_stencil_state.enable_depth_test = true;
        pci.depth_stencil_state.enable_depth_write = true;
        pci.target_info.color_target_descriptions = &color_target;
        pci.target_info.num_color_targets = 1;
        pci.target_info.has_depth_stencil_target = true;
        pci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        world_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);

        // Occluder-fade variant: same shaders, alpha-blended over what's already
        // drawn, depth-test LESS_EQUAL (its own depth is in the buffer is NOT —
        // faded meshes are skipped in the opaque pass) and depth-write OFF so it
        // never hides later draws. Mirrors the DX12 pso_blend_ (forward_pass).
        color_target.blend_state.enable_blend = true;
        color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pci.depth_stencil_state.enable_depth_write = false;
        world_blend_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);

        SDL_ReleaseGPUShader(gpu, vs);
        SDL_ReleaseGPUShader(gpu, fs);
        if (!world_pipeline_) {
            HD2D_ERROR("geometry: world pipeline failed: {}", SDL_GetError());
            return false;
        }
        if (!world_blend_pipeline_)
            HD2D_ERROR("geometry: world blend pipeline failed: {}", SDL_GetError());
    }

    // ---- billboard pipeline (textured, alpha-tested, depth-tested) ----
    {
        SDL_GPUShader* vs = make_shader(gpu, kBillboardMSL, "bb_vs",
                                        SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        SDL_GPUShader* fs = make_shader(gpu, kBillboardMSL, "bb_fs",
                                        SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        if (vs && fs) {
            SDL_GPUVertexBufferDescription vbd{};
            vbd.slot = 0;
            vbd.pitch = sizeof(float) * 5;   // pos.xyz + uv.xy
            vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

            SDL_GPUVertexAttribute attrs[2]{};
            attrs[0].location = 0;
            attrs[0].buffer_slot = 0;
            attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attrs[0].offset = 0;
            attrs[1].location = 1;
            attrs[1].buffer_slot = 0;
            attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attrs[1].offset = sizeof(float) * 3;

            SDL_GPUColorTargetDescription color_target{};
            color_target.format = swap_fmt;

            SDL_GPUGraphicsPipelineCreateInfo pci{};
            pci.vertex_shader = vs;
            pci.fragment_shader = fs;
            pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
            pci.vertex_input_state.num_vertex_buffers = 1;
            pci.vertex_input_state.vertex_attributes = attrs;
            pci.vertex_input_state.num_vertex_attributes = 2;
            pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
            pci.depth_stencil_state.enable_depth_test = true;
            pci.depth_stencil_state.enable_depth_write = true;
            pci.target_info.color_target_descriptions = &color_target;
            pci.target_info.num_color_targets = 1;
            pci.target_info.has_depth_stencil_target = true;
            pci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

            billboard_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
            if (!billboard_pipeline_)
                HD2D_ERROR("geometry: billboard pipeline failed: {}", SDL_GetError());
        }
        if (vs) SDL_ReleaseGPUShader(gpu, vs);
        if (fs) SDL_ReleaseGPUShader(gpu, fs);

        // Shared unit quad: x in [-0.5,0.5], y in [0,1] (stands on ground), +Z.
        // uv top-left origin. Two triangles, no index buffer reuse needed but we
        // keep an index buffer for symmetry.
        const float quad[4][5] = {
            //  x      y     z     u     v
            { -0.5f, 1.0f, 0.0f, 0.0f, 0.0f },
            {  0.5f, 1.0f, 0.0f, 1.0f, 0.0f },
            {  0.5f, 0.0f, 0.0f, 1.0f, 1.0f },
            { -0.5f, 0.0f, 0.0f, 0.0f, 1.0f },
        };
        const uint32_t qidx[6] = {0, 1, 2, 0, 2, 3};
        SDL_GPUCommandBuffer* qcmd = SDL_AcquireGPUCommandBuffer(gpu);
        quad_vb_ = upload_buffer(gpu, qcmd, quad, sizeof(quad), SDL_GPU_BUFFERUSAGE_VERTEX);
        quad_ib_ = upload_buffer(gpu, qcmd, qidx, sizeof(qidx), SDL_GPU_BUFFERUSAGE_INDEX);
        SDL_SubmitGPUCommandBuffer(qcmd);

        SDL_GPUSamplerCreateInfo sci{};
        sci.min_filter = SDL_GPU_FILTER_NEAREST;   // pixel art
        sci.mag_filter = SDL_GPU_FILTER_NEAREST;
        sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = SDL_CreateGPUSampler(gpu, &sci);
    }

    // ---- sun shadow caster pipeline (depth-only, no color target) ----
    // Mirrors shadow_pass.cpp: slope-scaled depth bias carved into the pipeline
    // to fight acne on the low-poly slopes, drawn with the same pos+normal layout
    // as the world mesh so the cached live VBs feed it directly.
    {
        SDL_GPUShader* vs = make_shader(gpu, kShadowMSL, "sh_vs",
                                        SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        SDL_GPUShader* fs = make_shader(gpu, kShadowMSL, "sh_fs",
                                        SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
        if (vs && fs) {
            SDL_GPUVertexBufferDescription vbd{};
            vbd.slot = 0;
            vbd.pitch = sizeof(PosNormal);
            vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

            SDL_GPUVertexAttribute attrs[2]{};
            attrs[0].location = 0;
            attrs[0].buffer_slot = 0;
            attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attrs[0].offset = 0;
            attrs[1].location = 1;
            attrs[1].buffer_slot = 0;
            attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attrs[1].offset = sizeof(float) * 3;

            SDL_GPUGraphicsPipelineCreateInfo pci{};
            pci.vertex_shader = vs;
            pci.fragment_shader = fs;   // trivial: 0 color targets, depth only
            pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
            pci.vertex_input_state.num_vertex_buffers = 1;
            pci.vertex_input_state.vertex_attributes = attrs;
            pci.vertex_input_state.num_vertex_attributes = 2;
            pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            pci.rasterizer_state.enable_depth_bias = true;
            pci.rasterizer_state.depth_bias_constant_factor = 1.25f;
            pci.rasterizer_state.depth_bias_slope_factor = 2.75f;
            pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
            pci.depth_stencil_state.enable_depth_test = true;
            pci.depth_stencil_state.enable_depth_write = true;
            pci.target_info.num_color_targets = 0;
            pci.target_info.has_depth_stencil_target = true;
            pci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

            shadow_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
            if (!shadow_pipeline_)
                HD2D_ERROR("geometry: shadow pipeline failed: {}", SDL_GetError());
        }
        if (vs) SDL_ReleaseGPUShader(gpu, vs);
        if (fs) SDL_ReleaseGPUShader(gpu, fs);

        // PCF comparison sampler: linear + clamp + LESS compare. Fragments deeper
        // than the stored depth (sc.z - bias) read 0 = shadowed.
        SDL_GPUSamplerCreateInfo ssi{};
        ssi.min_filter = SDL_GPU_FILTER_LINEAR;
        ssi.mag_filter = SDL_GPU_FILTER_LINEAR;
        ssi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        ssi.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        ssi.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        ssi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        ssi.enable_compare = true;
        ssi.compare_op = SDL_GPU_COMPAREOP_LESS;
        shadow_sampler_ = SDL_CreateGPUSampler(gpu, &ssi);

        // Allocate the depth map up front so the world pipeline always has a
        // depth2d to bind (even the no-shadow fallback path samples slot 0).
        shadow_target(gpu);
    }

    // ---- standalone fallback meshes (loaded from the .glb directly) ----
    // This path consumes only the CPU mesh copy (prim.cpu.*) and builds its own
    // SDL_GPU buffers, so the rhi handle table load_gltf fills is unused here —
    // pass a throwaway table (on SDL_GPU the submesh uploads are stubs anyway).
    LoadedScene scene;
    Dx12ResourceTable scratch_table;
    if (load_gltf(dev, scratch_table, glb_path, scene)) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
        bool have_bounds = false;
        float bmn[3] = {1e9f, 1e9f, 1e9f}, bmx[3] = {-1e9f, -1e9f, -1e9f};

        for (const LoadedNodeEx& node : scene.nodes) {
            for (const LoadedPrimitive& prim : node.prims) {
                const std::vector<Float3>& pos = prim.cpu.positions;
                const std::vector<uint32_t>& idx = prim.cpu.indices;
                if (pos.empty() || idx.size() < 3) continue;

                std::vector<PosNormal> verts;
                smooth_normals(pos, idx, verts);

                Node n;
                n.vb = upload_buffer(gpu, cmd, verts.data(),
                                     static_cast<uint32_t>(verts.size() * sizeof(PosNormal)),
                                     SDL_GPU_BUFFERUSAGE_VERTEX);
                n.ib = upload_buffer(gpu, cmd, idx.data(),
                                     static_cast<uint32_t>(idx.size() * sizeof(uint32_t)),
                                     SDL_GPU_BUFFERUSAGE_INDEX);
                if (!n.vb || !n.ib) {
                    if (n.vb) SDL_ReleaseGPUBuffer(gpu, n.vb);
                    if (n.ib) SDL_ReleaseGPUBuffer(gpu, n.ib);
                    continue;
                }
                n.index_count = static_cast<uint32_t>(idx.size());
                n.model = node.world;
                if (prim.material >= 0 &&
                    prim.material < static_cast<int>(scene.materials.size())) {
                    const LoadedMaterial& m = scene.materials[prim.material];
                    n.color[0] = m.base_color[0];
                    n.color[1] = m.base_color[1];
                    n.color[2] = m.base_color[2];
                }
                nodes_.push_back(n);

                const dx::XMMATRIX wm = dx::XMLoadFloat4x4(&node.world);
                for (const Float3& p : pos) {
                    dx::XMFLOAT4 wp;
                    dx::XMStoreFloat4(&wp,
                        dx::XMVector4Transform(dx::XMVectorSet(p.x, p.y, p.z, 1.0f), wm));
                    const float c[3] = {wp.x, wp.y, wp.z};
                    for (int k = 0; k < 3; ++k) {
                        bmn[k] = std::min(bmn[k], c[k]);
                        bmx[k] = std::max(bmx[k], c[k]);
                    }
                    have_bounds = true;
                }
            }
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_WaitForGPUIdle(gpu);
        if (have_bounds) {
            bmin_ = Float3{bmn[0], bmn[1], bmn[2]};
            bmax_ = Float3{bmx[0], bmx[1], bmx[2]};
        }
    } else {
        HD2D_WARN("geometry: standalone fallback load_gltf failed for {}", glb_path);
    }

    HD2D_INFO("geometry pass ready: {} fallback nodes from {} (live ECS path active)",
              nodes_.size(), glb_path);
    return world_pipeline_ != nullptr;
}

SDL_GPUTexture* GeometryPass::depth_target(SDL_GPUDevice* gpu, uint32_t width, uint32_t height) {
    if (depth_ && depth_w_ == width && depth_h_ == height) return depth_;
    if (depth_) { SDL_ReleaseGPUTexture(gpu, depth_); depth_ = nullptr; }
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    tci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    tci.width = width;
    tci.height = height;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depth_ = SDL_CreateGPUTexture(gpu, &tci);
    depth_w_ = width;
    depth_h_ = height;
    return depth_;
}

SDL_GPUTexture* GeometryPass::shadow_target(SDL_GPUDevice* gpu) {
    if (shadow_tex_) return shadow_tex_;
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    // SAMPLER so the geometry fragment shader can read it back as a depth2d.
    tci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = kShadowSize;
    tci.height = kShadowSize;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    shadow_tex_ = SDL_CreateGPUTexture(gpu, &tci);
    if (!shadow_tex_)
        HD2D_ERROR("geometry: shadow map texture failed: {}", SDL_GetError());
    return shadow_tex_;
}

void GeometryPass::flush_mesh_cache(SDL_GPUDevice* gpu) {
    for (auto& [k, cm] : mesh_cache_) {
        if (cm.vb) SDL_ReleaseGPUBuffer(gpu, cm.vb);
        if (cm.ib) SDL_ReleaseGPUBuffer(gpu, cm.ib);
    }
    mesh_cache_.clear();
}

void GeometryPass::render_shadow_map(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                                     SDL_GPURenderPass* pass, const LiveScene& scene) {
    if (scene.map_generation != seen_map_generation_) {   // 맵 스왑: 포인터-키 캐시 무효
        flush_mesh_cache(gpu);
        seen_map_generation_ = scene.map_generation;
    }
    if (!shadow_pipeline_ || !pass || !scene.shadow_active) return;

    const dx::XMMATRIX svp = dx::XMLoadFloat4x4(&scene.sun_view_proj);

    SDL_GPUViewport vp{};
    vp.w = static_cast<float>(kShadowSize);
    vp.h = static_cast<float>(kShadowSize);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &vp);
    SDL_BindGPUGraphicsPipeline(pass, shadow_pipeline_);

    for (const LiveMesh& m : scene.meshes) {
        const CachedMesh* cm = mesh_for(gpu, m);
        if (!cm) continue;
        const dx::XMMATRIX model = dx::XMLoadFloat4x4(&m.model);
        ShadowUniform u{};
        dx::XMStoreFloat4x4(&u.mvp, dx::XMMatrixMultiply(model, svp));
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));

        SDL_GPUBufferBinding vb{};
        vb.buffer = cm->vb;
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBufferBinding ib{};
        ib.buffer = cm->ib;
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, cm->index_count, 1, 0, 0, 0);
    }
}

const GeometryPass::CachedMesh* GeometryPass::mesh_for(SDL_GPUDevice* gpu, const LiveMesh& m) {
    if (!m.positions || !m.indices) return nullptr;
    const void* key = m.positions->data();
    auto it = mesh_cache_.find(key);
    if (it != mesh_cache_.end()) return &it->second;

    const std::vector<Float3>& pos = *m.positions;
    const std::vector<uint32_t>& idx = *m.indices;
    if (pos.empty() || idx.size() < 3) return nullptr;

    std::vector<PosNormal> verts;
    smooth_normals(pos, idx, verts);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    CachedMesh cm;
    cm.vb = upload_buffer(gpu, cmd, verts.data(),
                          static_cast<uint32_t>(verts.size() * sizeof(PosNormal)),
                          SDL_GPU_BUFFERUSAGE_VERTEX);
    cm.ib = upload_buffer(gpu, cmd, idx.data(),
                          static_cast<uint32_t>(idx.size() * sizeof(uint32_t)),
                          SDL_GPU_BUFFERUSAGE_INDEX);
    SDL_SubmitGPUCommandBuffer(cmd);
    if (!cm.vb || !cm.ib) {
        if (cm.vb) SDL_ReleaseGPUBuffer(gpu, cm.vb);
        if (cm.ib) SDL_ReleaseGPUBuffer(gpu, cm.ib);
        return nullptr;
    }
    cm.index_count = static_cast<uint32_t>(idx.size());
    auto [ins, ok] = mesh_cache_.emplace(key, cm);
    return &ins->second;
}

const GeometryPass::CachedTexture* GeometryPass::texture_for(SDL_GPUDevice* gpu,
                                                             const std::string& path) {
    auto it = tex_cache_.find(path);
    if (it != tex_cache_.end())
        return it->second.failed ? nullptr : &it->second;

    CachedTexture ct;
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        HD2D_WARN("geometry: sprite decode failed '{}': {}", path, stbi_failure_reason());
        ct.failed = true;
        tex_cache_.emplace(path, ct);
        return nullptr;
    }

    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = static_cast<uint32_t>(w);
    tci.height = static_cast<uint32_t>(h);
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(gpu, &tci);
    if (!tex) {
        HD2D_ERROR("geometry: SDL_CreateGPUTexture failed for '{}': {}", path, SDL_GetError());
        stbi_image_free(pixels);
        ct.failed = true;
        tex_cache_.emplace(path, ct);
        return nullptr;
    }

    const uint32_t bytes = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4;
    SDL_GPUTransferBufferCreateInfo xci{};
    xci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xci.size = bytes;
    SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(gpu, &xci);
    void* mapped = SDL_MapGPUTransferBuffer(gpu, xfer, false);
    SDL_memcpy(mapped, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(gpu, xfer);
    stbi_image_free(pixels);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = xfer;
    src.pixels_per_row = static_cast<uint32_t>(w);
    src.rows_per_layer = static_cast<uint32_t>(h);
    SDL_GPUTextureRegion region{};
    region.texture = tex;
    region.w = static_cast<uint32_t>(w);
    region.h = static_cast<uint32_t>(h);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu, xfer);

    ct.tex = tex;
    ct.width = static_cast<uint32_t>(w);
    ct.height = static_cast<uint32_t>(h);
    auto [ins, ok] = tex_cache_.emplace(path, ct);
    return &ins->second;
}

void GeometryPass::draw_live(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                             SDL_GPURenderPass* pass, const LiveScene& scene,
                             uint32_t width, uint32_t height) {
    if (scene.map_generation != seen_map_generation_) {   // 맵 스왑: 포인터-키 캐시 무효
        flush_mesh_cache(gpu);
        seen_map_generation_ = scene.map_generation;
    }
    if (!world_pipeline_ || !pass) return;

    const dx::XMMATRIX view = dx::XMLoadFloat4x4(&scene.view);
    const dx::XMMATRIX proj = dx::XMLoadFloat4x4(&scene.proj);
    const dx::XMMATRIX vp = dx::XMMatrixMultiply(view, proj);

    SDL_GPUViewport viewport{};
    viewport.w = static_cast<float>(width);
    viewport.h = static_cast<float>(height);
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);

    // The shadow map is sampled by the world fragment shader. It was rendered by
    // render_shadow_map in the device's prior depth-only pass; bind it (with the
    // PCF comparison sampler) so shadow_factor reads it. Whether shadows actually
    // apply is gated by scene.shadow_active via the uniform's shadow.w.
    const bool shadows = scene.shadow_active && shadow_tex_ && shadow_sampler_;

    // ---- world meshes ----
    // Meshes fading because they occlude the player (alpha<1) are skipped here
    // and redrawn blended AFTER the billboards so the character shows through.
    auto draw_mesh = [&](const LiveMesh& m) {
        const CachedMesh* cm = mesh_for(gpu, m);
        if (!cm) return;
        const dx::XMMATRIX model = dx::XMLoadFloat4x4(&m.model);
        const dx::XMMATRIX mvp = dx::XMMatrixMultiply(model, vp);

        VSUniform u{};
        dx::XMStoreFloat4x4(&u.mvp, mvp);
        u.model = m.model;
        u.sun_view_proj = scene.sun_view_proj;
        u.light_dir[0] = scene.light_dir[0];
        u.light_dir[1] = scene.light_dir[1];
        u.light_dir[2] = scene.light_dir[2];
        u.color[0] = m.color[0]; u.color[1] = m.color[1]; u.color[2] = m.color[2];
        u.color[3] = m.alpha;                                   // occluder fade
        u.ambient[0] = scene.ambient[0];
        u.ambient[1] = scene.ambient[1];
        u.ambient[2] = scene.ambient[2];
        u.shadow[0] = 1.0f / static_cast<float>(kShadowSize);  // texel
        u.shadow[1] = 0.0015f;                                  // depth bias
        u.shadow[2] = 0.05f;                                    // normal-offset
        u.shadow[3] = shadows ? 1.0f : 0.0f;                    // active
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));

        SDL_GPUBufferBinding vb{};
        vb.buffer = cm->vb;
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBufferBinding ib{};
        ib.buffer = cm->ib;
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, cm->index_count, 1, 0, 0, 0);
    };

    SDL_BindGPUGraphicsPipeline(pass, world_pipeline_);
    if (shadow_tex_ && shadow_sampler_) {
        SDL_GPUTextureSamplerBinding sb{};
        sb.texture = shadow_tex_;
        sb.sampler = shadow_sampler_;
        SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
    }
    bool any_faded = false;
    for (const LiveMesh& m : scene.meshes) {
        if (m.alpha < 0.999f && world_blend_pipeline_) { any_faded = true; continue; }
        draw_mesh(m);
    }

    // ---- billboards ----
    if (billboard_pipeline_ && quad_vb_ && quad_ib_ && sampler_ && !scene.billboards.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, billboard_pipeline_);
        SDL_GPUBufferBinding vb{};
        vb.buffer = quad_vb_;
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBufferBinding ib{};
        ib.buffer = quad_ib_;
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        const float kDeg2Rad = 3.14159265358979f / 180.0f;
        for (const LiveBillboard& b : scene.billboards) {
            const CachedTexture* ct = texture_for(gpu, b.sheet_path);
            if (!ct || !ct->tex) continue;

            // model = scale(world_height * squash) * rotateY(yaw) * translate(pos)
            // 페이퍼 스쿼시&스트레치: x=가로, y=세로 비균일(발밑 y=0 고정). z=height(뎁스).
            const dx::XMMATRIX model =
                dx::XMMatrixScaling(b.world_height * b.scale_x, b.world_height * b.scale_y,
                                    b.world_height) *
                dx::XMMatrixRotationY(b.yaw_deg * kDeg2Rad) *
                dx::XMMatrixTranslation(b.position.x, b.position.y, b.position.z);
            const dx::XMMATRIX mvp = dx::XMMatrixMultiply(model, vp);

            BBUniform u{};
            dx::XMStoreFloat4x4(&u.mvp, mvp);
            u.uv_off_scale[0] = b.uv_offset[0];
            u.uv_off_scale[1] = b.uv_offset[1];
            u.uv_off_scale[2] = b.uv_scale[0];
            u.uv_off_scale[3] = b.uv_scale[1];
            u.tint[0] = b.tint[0]; u.tint[1] = b.tint[1]; u.tint[2] = b.tint[2];
            SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));

            SDL_GPUTextureSamplerBinding tsb{};
            tsb.texture = ct->tex;
            tsb.sampler = sampler_;
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);

            SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
        }
    }

    // ---- faded occluders (translucent, over the player billboard) ----
    if (any_faded) {
        SDL_BindGPUGraphicsPipeline(pass, world_blend_pipeline_);
        if (shadow_tex_ && shadow_sampler_) {
            SDL_GPUTextureSamplerBinding sb{};
            sb.texture = shadow_tex_;
            sb.sampler = shadow_sampler_;
            SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
        }
        for (const LiveMesh& m : scene.meshes)
            if (m.alpha < 0.999f) draw_mesh(m);
    }
}

void GeometryPass::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
                        uint32_t width, uint32_t height) {
    if (!world_pipeline_ || nodes_.empty() || !pass) return;

    // Standalone fallback: orbit camera framing the whole map bounds.
    const Float3 center{(bmin_.x + bmax_.x) * 0.5f, (bmin_.y + bmax_.y) * 0.5f,
                        (bmin_.z + bmax_.z) * 0.5f};
    const float ext = 0.5f * std::sqrt(
        (bmax_.x - bmin_.x) * (bmax_.x - bmin_.x) +
        (bmax_.y - bmin_.y) * (bmax_.y - bmin_.y) +
        (bmax_.z - bmin_.z) * (bmax_.z - bmin_.z));

    OrbitCamera cam;
    cam.set_target(center);
    cam.set_distance(std::max(6.0f, ext * 1.4f));
    cam.set_yaw(35.0f);
    cam.far_z = std::max(200.0f, ext * 6.0f);
    cam.update(0.0f, 0.0f, 0.0f);

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const dx::XMMATRIX view = cam.view();
    const dx::XMMATRIX proj = cam.proj(aspect);
    const dx::XMMATRIX vp = dx::XMMatrixMultiply(view, proj);

    SDL_GPUViewport viewport{};
    viewport.w = static_cast<float>(width);
    viewport.h = static_cast<float>(height);
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);
    SDL_BindGPUGraphicsPipeline(pass, world_pipeline_);
    // The world fragment shader binds slot 0 to a depth2d; bind the shadow map
    // even in the fallback (shadows disabled via shadow.w = 0).
    if (shadow_tex_ && shadow_sampler_) {
        SDL_GPUTextureSamplerBinding sb{};
        sb.texture = shadow_tex_;
        sb.sampler = shadow_sampler_;
        SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
    }

    for (const Node& n : nodes_) {
        const dx::XMMATRIX model = dx::XMLoadFloat4x4(&n.model);
        const dx::XMMATRIX mvp = dx::XMMatrixMultiply(model, vp);

        VSUniform u{};
        dx::XMStoreFloat4x4(&u.mvp, mvp);
        u.model = n.model;
        u.light_dir[0] = 0.4f; u.light_dir[1] = 0.8f; u.light_dir[2] = -0.45f;
        u.color[0] = n.color[0]; u.color[1] = n.color[1]; u.color[2] = n.color[2];
        u.ambient[0] = 0.18f; u.ambient[1] = 0.20f; u.ambient[2] = 0.26f;
        u.shadow[3] = 0.0f;  // no shadows in the standalone fallback
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));

        SDL_GPUBufferBinding vb{};
        vb.buffer = n.vb;
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBufferBinding ib{};
        ib.buffer = n.ib;
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, n.index_count, 1, 0, 0, 0);
    }
}

void GeometryPass::shutdown(SDL_GPUDevice* gpu) {
    if (!gpu) {
        nodes_.clear();
        mesh_cache_.clear();
        tex_cache_.clear();
        world_pipeline_ = nullptr;
        billboard_pipeline_ = nullptr;
        shadow_pipeline_ = nullptr;
        shadow_sampler_ = nullptr;
        shadow_tex_ = nullptr;
        depth_ = nullptr;
        return;
    }
    for (Node& n : nodes_) {
        if (n.vb) SDL_ReleaseGPUBuffer(gpu, n.vb);
        if (n.ib) SDL_ReleaseGPUBuffer(gpu, n.ib);
    }
    nodes_.clear();
    for (auto& [k, cm] : mesh_cache_) {
        if (cm.vb) SDL_ReleaseGPUBuffer(gpu, cm.vb);
        if (cm.ib) SDL_ReleaseGPUBuffer(gpu, cm.ib);
    }
    mesh_cache_.clear();
    for (auto& [k, ct] : tex_cache_) {
        if (ct.tex) SDL_ReleaseGPUTexture(gpu, ct.tex);
    }
    tex_cache_.clear();
    if (quad_vb_) { SDL_ReleaseGPUBuffer(gpu, quad_vb_); quad_vb_ = nullptr; }
    if (quad_ib_) { SDL_ReleaseGPUBuffer(gpu, quad_ib_); quad_ib_ = nullptr; }
    if (sampler_) { SDL_ReleaseGPUSampler(gpu, sampler_); sampler_ = nullptr; }
    if (shadow_sampler_) { SDL_ReleaseGPUSampler(gpu, shadow_sampler_); shadow_sampler_ = nullptr; }
    if (shadow_tex_) { SDL_ReleaseGPUTexture(gpu, shadow_tex_); shadow_tex_ = nullptr; }
    if (depth_) { SDL_ReleaseGPUTexture(gpu, depth_); depth_ = nullptr; }
    if (world_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, world_pipeline_); world_pipeline_ = nullptr; }
    if (world_blend_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, world_blend_pipeline_); world_blend_pipeline_ = nullptr; }
    if (billboard_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, billboard_pipeline_); billboard_pipeline_ = nullptr; }
    if (shadow_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, shadow_pipeline_); shadow_pipeline_ = nullptr; }
}

} // namespace hd2d
