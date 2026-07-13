// ============================================================================
// sdlgpu/post_pass.cpp — HD-2D post chain on SDL_GPU (rung 2).
//
// Direct MSL port of post_chain.cpp / shaders/post.hlsl: a bloom pyramid
// (Jimenez 13-tap downsample with a soft-knee bright pass on the first tap, then
// a 9-tap tent upsample blended additively up the chain) plus an AgX filmic view
// transform. The geometry pass renders the lit world into an RGBA16F HDR target;
// this resolves it to the swapchain.
//
// Uniforms are pushed via SDL_PushGPUFragmentUniformData (no descriptor heaps).
// The fullscreen triangle is generated from the vertex id (no vertex buffer).
// ============================================================================

#include "renderer/sdlgpu/post_pass.h"

#include "core/log_compat.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace hd2d {

namespace {

constexpr SDL_GPUTextureFormat kHdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// Matches post.hlsl's PostConstants (b0). 16-byte aligned for the std140-ish
// MSL constant buffer layout.
struct PostConstants {
    float exposure;
    float bloom_intensity;
    uint32_t flags;      // bit0 AgX, bit1 input-linear (tonemap), bit2 first bloom down
    float threshold;
    float knee;
    float texel_x;       // texel size of the SOURCE being sampled
    float texel_y;
    float _pad;
};

// ---- fullscreen-triangle vertex shader (shared by all post passes) ----------
const char* kPostVS = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 pos [[position]];
    float2 uv;
};

vertex VSOut post_vs(uint id [[vertex_id]]) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)MSL";

// ---- bloom downsample: Jimenez 13-tap + soft-knee bright pass on tap 0 -------
const char* kBloomDownFS = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct PostConstants {
    float exposure;
    float bloom_intensity;
    uint  flags;
    float threshold;
    float knee;
    float2 texel;
    float _pad;
};

struct VSOut { float4 pos [[position]]; float2 uv; };

float3 bloom_prefilter(float3 c, constant PostConstants& u) {
    float brightness = max(c.r, max(c.g, c.b));
    float soft = brightness - u.threshold + u.knee;
    soft = clamp(soft, 0.0, 2.0 * u.knee);
    soft = soft * soft / (4.0 * u.knee + 1e-4);
    float contribution = max(soft, brightness - u.threshold) / max(brightness, 1e-4);
    return c * contribution;
}

fragment float4 bloom_down_fs(VSOut i [[stage_in]],
                              constant PostConstants& u [[buffer(0)]],
                              texture2d<float> src [[texture(0)]],
                              sampler smp [[sampler(0)]]) {
    float2 uv = i.uv;
    float2 t = u.texel;
    float3 a = src.sample(smp, uv + t * float2(-2, -2)).rgb;
    float3 b = src.sample(smp, uv + t * float2( 0, -2)).rgb;
    float3 c = src.sample(smp, uv + t * float2( 2, -2)).rgb;
    float3 d = src.sample(smp, uv + t * float2(-2,  0)).rgb;
    float3 e = src.sample(smp, uv).rgb;
    float3 f = src.sample(smp, uv + t * float2( 2,  0)).rgb;
    float3 g = src.sample(smp, uv + t * float2(-2,  2)).rgb;
    float3 h = src.sample(smp, uv + t * float2( 0,  2)).rgb;
    float3 ii = src.sample(smp, uv + t * float2( 2,  2)).rgb;
    float3 j = src.sample(smp, uv + t * float2(-1, -1)).rgb;
    float3 k = src.sample(smp, uv + t * float2( 1, -1)).rgb;
    float3 l = src.sample(smp, uv + t * float2(-1,  1)).rgb;
    float3 m = src.sample(smp, uv + t * float2( 1,  1)).rgb;

    float3 col = e * 0.125;
    col += (a + c + g + ii) * 0.03125;
    col += (b + d + f + h) * 0.0625;
    col += (j + k + l + m) * 0.125;

    if (u.flags & 4) col = bloom_prefilter(col, u);
    return float4(col, 1.0);
}
)MSL";

// ---- bloom upsample: 9-tap tent (the pipeline blends additively) ------------
const char* kBloomUpFS = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct PostConstants {
    float exposure;
    float bloom_intensity;
    uint  flags;
    float threshold;
    float knee;
    float2 texel;
    float _pad;
};

struct VSOut { float4 pos [[position]]; float2 uv; };

fragment float4 bloom_up_fs(VSOut i [[stage_in]],
                            constant PostConstants& u [[buffer(0)]],
                            texture2d<float> src [[texture(0)]],
                            sampler smp [[sampler(0)]]) {
    float2 uv = i.uv;
    float2 t = u.texel;
    float3 col = src.sample(smp, uv + t * float2(-1, -1)).rgb;
    col += src.sample(smp, uv + t * float2(0, -1)).rgb * 2.0;
    col += src.sample(smp, uv + t * float2(1, -1)).rgb;
    col += src.sample(smp, uv + t * float2(-1, 0)).rgb * 2.0;
    col += src.sample(smp, uv).rgb * 4.0;
    col += src.sample(smp, uv + t * float2(1, 0)).rgb * 2.0;
    col += src.sample(smp, uv + t * float2(-1, 1)).rgb;
    col += src.sample(smp, uv + t * float2(0, 1)).rgb * 2.0;
    col += src.sample(smp, uv + t * float2(1, 1)).rgb;
    return float4(col / 16.0, 1.0);
}
)MSL";

// ---- tonemap: scene + bloom -> AgX (or sRGB) -> swapchain -------------------
// HLSL's mul(M, v) is a row-major matrix * column vector (dot of each ROW with
// v). MSL's M*v uses M's COLUMNS, so we replicate HLSL with an explicit
// row-dot helper to keep the published AgX matrices byte-identical.
const char* kTonemapFS = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct PostConstants {
    float exposure;
    float bloom_intensity;
    uint  flags;
    float threshold;
    float knee;
    float2 texel;
    float _pad;
};

struct VSOut { float4 pos [[position]]; float2 uv; };

float3 srgb_encode(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return select(hi, lo, c <= 0.0031308);
}

float3 agx_default_contrast(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
           0.4298 * x2 + 0.1191 * x - 0.00232;
}

// mul(M, v) HLSL-style: dot each row of M with v. The rows are the float3
// triples exactly as listed in post.hlsl.
float3 mul_rows(float3 r0, float3 r1, float3 r2, float3 v) {
    return float3(dot(r0, v), dot(r1, v), dot(r2, v));
}

float3 agx_tonemap(float3 val) {
    float3 m0 = float3(0.842479062253094,  0.0784335999999992, 0.0792237451477643);
    float3 m1 = float3(0.0423282422610123, 0.878468636469772,  0.0791661274605434);
    float3 m2 = float3(0.0423756549057051, 0.0784336,          0.879142973793104);
    float3 i0 = float3( 1.19687900512017,   -0.0980208811401368, -0.0990297440797205);
    float3 i1 = float3(-0.0528968517574562,  1.15190312990417,   -0.0989611768448433);
    float3 i2 = float3(-0.0529716355144438, -0.0980434501171241,  1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;

    val = mul_rows(m0, m1, m2, val);
    val = clamp(log2(max(val, 1e-10)), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    val = agx_default_contrast(val);
    val = mul_rows(i0, i1, i2, val);
    return saturate(val);
}

fragment float4 tonemap_fs(VSOut i [[stage_in]],
                           constant PostConstants& u [[buffer(0)]],
                           texture2d<float> scene [[texture(0)]],
                           texture2d<float> bloom [[texture(1)]],
                           sampler smp [[sampler(0)]]) {
    float3 hdr = scene.sample(smp, i.uv).rgb;
    hdr += bloom.sample(smp, i.uv).rgb * u.bloom_intensity;

    float3 c = hdr * u.exposure;
    if (u.flags & 1) return float4(agx_tonemap(c), 1.0);
    return float4(srgb_encode(c), 1.0);
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
    if (!sh) HD2D_ERROR("post: SDL_CreateGPUShader({}) failed: {}", entry, SDL_GetError());
    return sh;
}

SDL_GPUTexture* make_color_target(SDL_GPUDevice* gpu, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = kHdrFormat;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = w;
    tci.height = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(gpu, &tci);
}

} // namespace

bool PostPass::init(SDL_GPUDevice* gpu, SDL_Window* window) {
    if (!gpu || !window) return false;
    if ((SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) == 0) return false;

    const SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(gpu, window);

    SDL_GPUShader* vs = make_shader(gpu, kPostVS, "post_vs",
                                    SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* tone = make_shader(gpu, kTonemapFS, "tonemap_fs",
                                      SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2);
    SDL_GPUShader* down = make_shader(gpu, kBloomDownFS, "bloom_down_fs",
                                      SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUShader* up = make_shader(gpu, kBloomUpFS, "bloom_up_fs",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs || !tone || !down || !up) {
        if (vs) SDL_ReleaseGPUShader(gpu, vs);
        if (tone) SDL_ReleaseGPUShader(gpu, tone);
        if (down) SDL_ReleaseGPUShader(gpu, down);
        if (up) SDL_ReleaseGPUShader(gpu, up);
        return false;
    }

    auto base_pci = [&](SDL_GPUShader* fs) {
        SDL_GPUGraphicsPipelineCreateInfo pci{};
        pci.vertex_shader = vs;
        pci.fragment_shader = fs;
        pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        return pci;
    };

    // Tonemap -> swapchain (no blend, no depth).
    {
        SDL_GPUColorTargetDescription ct{};
        ct.format = swap_fmt;
        SDL_GPUGraphicsPipelineCreateInfo pci = base_pci(tone);
        pci.target_info.color_target_descriptions = &ct;
        pci.target_info.num_color_targets = 1;
        tonemap_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
        if (!tonemap_pipeline_)
            HD2D_ERROR("post: tonemap pipeline failed: {}", SDL_GetError());
    }

    // Bloom downsample -> HDR mip (no blend).
    {
        SDL_GPUColorTargetDescription ct{};
        ct.format = kHdrFormat;
        SDL_GPUGraphicsPipelineCreateInfo pci = base_pci(down);
        pci.target_info.color_target_descriptions = &ct;
        pci.target_info.num_color_targets = 1;
        bloom_down_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
        if (!bloom_down_pipeline_)
            HD2D_ERROR("post: bloom down pipeline failed: {}", SDL_GetError());
    }

    // Bloom upsample -> additive tent onto the lower mip.
    {
        SDL_GPUColorTargetDescription ct{};
        ct.format = kHdrFormat;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pci = base_pci(up);
        pci.target_info.color_target_descriptions = &ct;
        pci.target_info.num_color_targets = 1;
        bloom_up_pipeline_ = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
        if (!bloom_up_pipeline_)
            HD2D_ERROR("post: bloom up pipeline failed: {}", SDL_GetError());
    }

    SDL_ReleaseGPUShader(gpu, vs);
    SDL_ReleaseGPUShader(gpu, tone);
    SDL_ReleaseGPUShader(gpu, down);
    SDL_ReleaseGPUShader(gpu, up);

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_ = SDL_CreateGPUSampler(gpu, &sci);

    HD2D_INFO("sdlgpu post pass ready (bloom {} mips + AgX tonemap)", kBloomMips);
    return tonemap_pipeline_ != nullptr;
}

SDL_GPUTexture* PostPass::hdr_target(SDL_GPUDevice* gpu, uint32_t width, uint32_t height) {
    if (hdr_ && hdr_w_ == width && hdr_h_ == height) return hdr_;
    if (hdr_) { SDL_ReleaseGPUTexture(gpu, hdr_); hdr_ = nullptr; }
    hdr_ = make_color_target(gpu, width, height);
    hdr_w_ = width;
    hdr_h_ = height;
    if (!hdr_) HD2D_ERROR("post: HDR scene target failed ({}x{})", width, height);
    return hdr_;
}

bool PostPass::ensure_bloom(SDL_GPUDevice* gpu, uint32_t width, uint32_t height) {
    if (bloom_[0] && width == bloom_base_w_ && height == bloom_base_h_) return true;
    for (uint32_t i = 0; i < kBloomMips; ++i)
        if (bloom_[i]) { SDL_ReleaseGPUTexture(gpu, bloom_[i]); bloom_[i] = nullptr; }

    uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    for (uint32_t i = 0; i < kBloomMips; ++i) {
        bloom_w_[i] = w;
        bloom_h_[i] = h;
        bloom_[i] = make_color_target(gpu, w, h);
        if (!bloom_[i]) {
            HD2D_ERROR("post: bloom mip {} failed ({}x{})", i, w, h);
            return false;
        }
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    bloom_base_w_ = width;
    bloom_base_h_ = height;
    return true;
}

void PostPass::execute(SDL_GPUDevice* gpu, SDL_GPUCommandBuffer* cmd,
                       SDL_GPUTexture* swap_tex, uint32_t width, uint32_t height,
                       const PostParams& params) {
    if (!tonemap_pipeline_ || !hdr_ || !swap_tex) return;

    const bool bloom_on = params.bloom && bloom_down_pipeline_ && bloom_up_pipeline_ &&
                          ensure_bloom(gpu, width, height);

    PostConstants pc{};
    pc.exposure = params.exposure;
    pc.bloom_intensity = bloom_on ? params.bloom_intensity : 0.0f;
    pc.threshold = params.bloom_threshold;
    pc.knee = params.bloom_knee;

    auto draw_fullscreen = [&](SDL_GPURenderPass* pass) {
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    };
    auto set_viewport = [&](SDL_GPURenderPass* pass, uint32_t w, uint32_t h) {
        SDL_GPUViewport vp{};
        vp.w = static_cast<float>(w);
        vp.h = static_cast<float>(h);
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;
        SDL_SetGPUViewport(pass, &vp);
    };

    if (bloom_on) {
        // Downsample chain: scene -> mip0 (thresholded) -> mip1 .. mipN.
        for (uint32_t i = 0; i < kBloomMips; ++i) {
            SDL_GPUColorTargetInfo ci{};
            ci.texture = bloom_[i];
            ci.load_op = SDL_GPU_LOADOP_DONT_CARE;
            ci.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
            if (!pass) continue;
            set_viewport(pass, bloom_w_[i], bloom_h_[i]);
            SDL_BindGPUGraphicsPipeline(pass, bloom_down_pipeline_);

            SDL_GPUTexture* src = (i == 0) ? hdr_ : bloom_[i - 1];
            const uint32_t src_w = (i == 0) ? width : bloom_w_[i - 1];
            const uint32_t src_h = (i == 0) ? height : bloom_h_[i - 1];
            pc.flags = (params.agx ? 1u : 0u) | 2u | (i == 0 ? 4u : 0u);
            pc.texel_x = 1.0f / static_cast<float>(src_w);
            pc.texel_y = 1.0f / static_cast<float>(src_h);
            SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof(pc));

            SDL_GPUTextureSamplerBinding sb{};
            sb.texture = src;
            sb.sampler = linear_;
            SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
            draw_fullscreen(pass);
            SDL_EndGPURenderPass(pass);
        }

        // Upsample chain: tent-blur mip i+1 additively onto mip i.
        for (int i = static_cast<int>(kBloomMips) - 2; i >= 0; --i) {
            SDL_GPUColorTargetInfo ci{};
            ci.texture = bloom_[i];
            ci.load_op = SDL_GPU_LOADOP_LOAD;   // additive onto existing mip
            ci.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
            if (!pass) continue;
            set_viewport(pass, bloom_w_[i], bloom_h_[i]);
            SDL_BindGPUGraphicsPipeline(pass, bloom_up_pipeline_);

            pc.flags = (params.agx ? 1u : 0u) | 2u;
            pc.texel_x = 1.0f / static_cast<float>(bloom_w_[i + 1]);
            pc.texel_y = 1.0f / static_cast<float>(bloom_h_[i + 1]);
            SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof(pc));

            SDL_GPUTextureSamplerBinding sb{};
            sb.texture = bloom_[i + 1];
            sb.sampler = linear_;
            SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
            draw_fullscreen(pass);
            SDL_EndGPURenderPass(pass);
        }
    }

    // Tonemap scene (+ bloom mip0) -> swapchain.
    {
        SDL_GPUColorTargetInfo ci{};
        ci.texture = swap_tex;
        ci.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
        if (!pass) return;
        set_viewport(pass, width, height);
        SDL_BindGPUGraphicsPipeline(pass, tonemap_pipeline_);

        pc.flags = (params.agx ? 1u : 0u) | 2u;
        pc.bloom_intensity = bloom_on ? params.bloom_intensity : 0.0f;
        pc.texel_x = 1.0f / static_cast<float>(width);
        pc.texel_y = 1.0f / static_cast<float>(height);
        SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof(pc));

        // t0 = scene HDR, t1 = bloom mip0 (or scene again when bloom is off).
        SDL_GPUTextureSamplerBinding sb[2]{};
        sb[0].texture = hdr_;
        sb[0].sampler = linear_;
        sb[1].texture = bloom_on ? bloom_[0] : hdr_;
        sb[1].sampler = linear_;
        SDL_BindGPUFragmentSamplers(pass, 0, sb, 2);
        draw_fullscreen(pass);
        SDL_EndGPURenderPass(pass);
    }
}

void PostPass::shutdown(SDL_GPUDevice* gpu) {
    if (!gpu) {
        tonemap_pipeline_ = bloom_down_pipeline_ = bloom_up_pipeline_ = nullptr;
        linear_ = nullptr;
        hdr_ = nullptr;
        for (uint32_t i = 0; i < kBloomMips; ++i) bloom_[i] = nullptr;
        return;
    }
    if (hdr_) { SDL_ReleaseGPUTexture(gpu, hdr_); hdr_ = nullptr; }
    for (uint32_t i = 0; i < kBloomMips; ++i)
        if (bloom_[i]) { SDL_ReleaseGPUTexture(gpu, bloom_[i]); bloom_[i] = nullptr; }
    if (linear_) { SDL_ReleaseGPUSampler(gpu, linear_); linear_ = nullptr; }
    if (tonemap_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, tonemap_pipeline_); tonemap_pipeline_ = nullptr; }
    if (bloom_down_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, bloom_down_pipeline_); bloom_down_pipeline_ = nullptr; }
    if (bloom_up_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(gpu, bloom_up_pipeline_); bloom_up_pipeline_ = nullptr; }
}

} // namespace hd2d
