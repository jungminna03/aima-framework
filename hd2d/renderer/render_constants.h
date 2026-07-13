#pragma once

// CPU mirrors of the per-frame / per-draw HLSL cbuffers (shaders/standard.hlsl),
// split out of forward_pass.h so the GAME module can build them WITHOUT pulling in
// <d3d12.h> (PAL render keystone). These structs are plain data — only DirectXMath
// matrices + scalars, no D3D12 types. forward_pass.h #includes this and keeps the
// D3D12-typed MaterialSrvs renderer-side. alignas(256) keeps each in its own
// constant-buffer slot. Lights are SoA float4 arrays (FXC struct-array cbuffer
// layout is not byte-predictable; float4[] is exactly one row per element).

#include "core/math_compat.h"   // dx::XMFLOAT4X4 (DirectXMath — math facade, not D3D12)

#include <cstdint>

namespace hd2d {

struct alignas(256) FrameConstants {
    static constexpr uint32_t kMaxLights = 16;
    dx::XMFLOAT4X4 view_proj;
    float cam_pos[3];  float light_count;
    float ambient[3];  float _p0;                 // world ambient radiance
    dx::XMFLOAT4X4 sun_view_proj;                 // shadow-map transform
    // x = shadow texel size, y = depth bias, z = normal-offset (world units),
    // w = index of the shadowed sun light (-1 = shadows off).
    float shadow_params[4];
    float light_pos_range[kMaxLights][4];         // xyz pos, w range
    float light_dir_type[kMaxLights][4];          // xyz dir, w type
    float light_color_inner[kMaxLights][4];       // rgb color*W, w inner cos
    float light_outer[kMaxLights][4];             // x outer cos
};

struct alignas(256) DrawConstants {
    dx::XMFLOAT4X4 model;
    dx::XMFLOAT4X4 normal_mat;  // inverse-transpose of model (normals under scale)
    float base_color[4];
    float metallic;
    float roughness;
    float alpha_cutoff;
    uint32_t flags;        // bit0 nearest, bit1 clamp, bit2 has normal map
    float uv_offset[2];
    float uv_scale[2];
    float emissive[3];     // emissive factor * KHR emissive strength (linear)
    float _p4;
};

} // namespace hd2d
