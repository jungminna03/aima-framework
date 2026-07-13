// HD-2D post chain: bloom (down/up-sample pyramid) + fullscreen tonemap.
// The scene renders to a linear HDR target; this applies exposure, bloom, the
// view transform (AgX — Blender's default — or plain sRGB), and writes the
// UNORM backbuffer.

cbuffer Post : register(b0) {
    float gExposure;
    float gBloomIntensity;
    uint  gFlags;          // bit0 AgX, bit1 input-linear, bit2 first bloom down, bit3 DoF composite
    float gThreshold;      // bloom soft-knee threshold (scene-linear)
    float gKnee;
    float2 gTexel;         // texel size of the SOURCE being sampled
    float _pad;
    // --- 틸트시프트 DoF (스펙 2026-07-02; src/game/dof_math.h와 1:1) ---
    float gDofFocusDist;   // 카메라→초점(파티) m
    float gDofFocusRange;  // 완전 선명 반폭 m
    float gDofBlurRange;   // 선명→최대 블러 전이 m
    float gDofStrength;    // [0,1]
    float gDofBandCenter;  // 선명 밴드 중심 (uv.y, 0=상단)
    float gDofBandHalf;
    float gDofBandFeather;
    float gDofProtect;     // 밴드 항 깊이 보호 반경 m
    float gDofMaxCoc;      // 최대 블러 반경 (풀해상도 px)
    float gCamNear;
    float gCamFar;
    float _pad2;
};

Texture2D    gScene  : register(t0);   // scene HDR / bloom pyramid / DoF ping-pong source
Texture2D    gBloom  : register(t1);   // bloom result for the composite
Texture2D<float> gDepth : register(t2);// scene depth (R32_FLOAT view over R32_TYPELESS)
Texture2D    gDof    : register(t3);   // DoF result (rgb=blurred, a=CoC) for the composite
SamplerState gLinear : register(s0);
SamplerState gPoint  : register(s1);   // depth는 보간 없이

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle from the vertex id (no vertex buffer).
VSOut VSFullscreen(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 srgb_encode(float3 c) {
    c = saturate(c);
    // FXC (SM 5.1) has no select(); the ternary is componentwise on vectors.
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// ---------------------------------------------------------------------------
// AgX view transform (punctual approximation; matches Blender 4/5 "AgX, Look:
// None" closely). Reference: Troy Sobotka's AgX; polynomial fit by B. Wrensch.
// NOTE: float3x3(...) fills rows in HLSL; these matrices are written so that
// mul(M, v) applies the inset/outset as published (GLSL mat3 fills columns,
// hence the transposed listing relative to the original source).
// ---------------------------------------------------------------------------
float3 agx_default_contrast(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
           0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx_tonemap(float3 val) {
    const float3x3 agx_mat = float3x3(
        0.842479062253094,  0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772,  0.0791661274605434,
        0.0423756549057051, 0.0784336,          0.879142973793104);
    const float3x3 agx_mat_inv = float3x3(
         1.19687900512017,   -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562,  1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438, -0.0980434501171241,  1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;

    val = mul(agx_mat, val);
    val = clamp(log2(max(val, 1e-10)), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    val = agx_default_contrast(val);
    // Outset; output is display-encoded (no extra sRGB encode after this).
    val = mul(agx_mat_inv, val);
    return saturate(val);
}

// ---------------------------------------------------------------------------
// Bloom — Jimenez/CoD-style downsample + tent upsample.
// ---------------------------------------------------------------------------

// Quadratic soft knee (Unity/Eevee-legacy style threshold curve).
float3 bloom_prefilter(float3 c) {
    float brightness = max(c.r, max(c.g, c.b));
    float soft = brightness - gThreshold + gKnee;
    soft = clamp(soft, 0.0, 2.0 * gKnee);
    soft = soft * soft / (4.0 * gKnee + 1e-4);
    float contribution = max(soft, brightness - gThreshold) / max(brightness, 1e-4);
    return c * contribution;
}

// 13-tap downsample (Jimenez SIGGRAPH 2014) — stable, no pulsing.
float4 PSBloomDown(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float2 t = gTexel;
    float3 a = gScene.Sample(gLinear, uv + t * float2(-2, -2)).rgb;
    float3 b = gScene.Sample(gLinear, uv + t * float2( 0, -2)).rgb;
    float3 c = gScene.Sample(gLinear, uv + t * float2( 2, -2)).rgb;
    float3 d = gScene.Sample(gLinear, uv + t * float2(-2,  0)).rgb;
    float3 e = gScene.Sample(gLinear, uv).rgb;
    float3 f = gScene.Sample(gLinear, uv + t * float2( 2,  0)).rgb;
    float3 g = gScene.Sample(gLinear, uv + t * float2(-2,  2)).rgb;
    float3 h = gScene.Sample(gLinear, uv + t * float2( 0,  2)).rgb;
    float3 ii = gScene.Sample(gLinear, uv + t * float2( 2,  2)).rgb;
    float3 j = gScene.Sample(gLinear, uv + t * float2(-1, -1)).rgb;
    float3 k = gScene.Sample(gLinear, uv + t * float2( 1, -1)).rgb;
    float3 l = gScene.Sample(gLinear, uv + t * float2(-1,  1)).rgb;
    float3 m = gScene.Sample(gLinear, uv + t * float2( 1,  1)).rgb;

    float3 col = e * 0.125;
    col += (a + c + g + ii) * 0.03125;
    col += (b + d + f + h) * 0.0625;
    col += (j + k + l + m) * 0.125;

    if (gFlags & 4) col = bloom_prefilter(col);   // first tap from the scene
    return float4(col, 1.0);
}

// 9-tap tent upsample; the PSO blends this additively onto the lower mip.
float4 PSBloomUp(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float2 t = gTexel;
    float3 col = gScene.Sample(gLinear, uv + t * float2(-1, -1)).rgb;
    col += gScene.Sample(gLinear, uv + t * float2(0, -1)).rgb * 2.0;
    col += gScene.Sample(gLinear, uv + t * float2(1, -1)).rgb;
    col += gScene.Sample(gLinear, uv + t * float2(-1, 0)).rgb * 2.0;
    col += gScene.Sample(gLinear, uv).rgb * 4.0;
    col += gScene.Sample(gLinear, uv + t * float2(1, 0)).rgb * 2.0;
    col += gScene.Sample(gLinear, uv + t * float2(-1, 1)).rgb;
    col += gScene.Sample(gLinear, uv + t * float2(0, 1)).rgb * 2.0;
    col += gScene.Sample(gLinear, uv + t * float2(1, 1)).rgb;
    return float4(col / 16.0, 1.0);
}

// ---------------------------------------------------------------------------
// 틸트시프트 DoF (미니어처 룩, 스펙 2026-07-02). 하프해상도 3패스:
// PSDofCoc(색 다운샘플+CoC) → PSDofBlur(골든앵글 디스크 개더) → PSDofTent(정리).
// CoC 공식은 src/game/dof_math.h의 C++ 미러와 1:1 — 어느 쪽을 고치든 둘 다.
// ---------------------------------------------------------------------------

// 비선형 깊이 d[0,1] → 뷰공간 선형 거리(m). 표준 Z(clear=1=far).
float linearize_depth(float d) {
    return gCamNear * gCamFar / (gCamFar - d * (gCamFar - gCamNear));
}

// 하이브리드 CoC: 깊이 항 ∨ (화면 밴드 × 깊이 보호). [0,1].
// protect는 초점 깊이의 유닛이 화면 가장자리에 있어도 안 흐려지게 밴드 항만 죽인다.
float dof_coc(float z, float uv_y) {
    float dz = abs(z - gDofFocusDist);
    float coc_depth = saturate((dz - gDofFocusRange) / gDofBlurRange);
    float band = smoothstep(gDofBandHalf, gDofBandHalf + gDofBandFeather,
                            abs(uv_y - gDofBandCenter));
    float protect = saturate(dz / gDofProtect);
    return max(coc_depth, band * protect) * gDofStrength;
}

// 하프해상도: rgb = 씬 색 다운샘플, a = CoC.
float4 PSDofCoc(VSOut i) : SV_Target {
    float3 col = gScene.Sample(gLinear, i.uv).rgb;
    float  z = linearize_depth(gDepth.Sample(gPoint, i.uv));
    return float4(col, dof_coc(z, i.uv.y));
}

// 골든앵글 스파이럴 디스크 개더. 이웃 기여를 이웃 CoC/중심 CoC 비로 감쇠 —
// 선명한 이웃(초점의 캐릭터)이 흐린 배경으로 번지지 않는다(실루엣 보호,
// scatter-as-gather 근사; near/far 분리 없음은 v1 수용 한계).
float4 PSDofBlur(VSOut i) : SV_Target {
    float4 center = gScene.Sample(gLinear, i.uv);
    float R = center.a * gDofMaxCoc * 0.5;   // 하프해상도 px 반경
    if (R < 0.5) return center;
    float3 acc = center.rgb;
    float wsum = 1.0;
    const float kGolden = 2.39996323;
    [loop] for (int k = 1; k <= 32; ++k) {
        float r = R * sqrt(float(k) / 32.0);
        float ang = float(k) * kGolden;
        float2 off = float2(cos(ang), sin(ang)) * r * gTexel;
        float4 s = gScene.Sample(gLinear, i.uv + off);
        float w = saturate(s.a / max(center.a, 1e-3));
        acc += s.rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, center.a);
}

// 3×3 텐트 — 32탭 언더샘플링 노이즈 정리(CoC도 살짝 부드럽게 = 합성 마스크 페더).
float4 PSDofTent(VSOut i) : SV_Target {
    float2 t = gTexel;
    float4 c = gScene.Sample(gLinear, i.uv) * 4.0;
    c += gScene.Sample(gLinear, i.uv + float2(-t.x, 0)) * 2.0;
    c += gScene.Sample(gLinear, i.uv + float2( t.x, 0)) * 2.0;
    c += gScene.Sample(gLinear, i.uv + float2(0, -t.y)) * 2.0;
    c += gScene.Sample(gLinear, i.uv + float2(0,  t.y)) * 2.0;
    c += gScene.Sample(gLinear, i.uv + float2(-t.x, -t.y));
    c += gScene.Sample(gLinear, i.uv + float2( t.x, -t.y));
    c += gScene.Sample(gLinear, i.uv + float2(-t.x,  t.y));
    c += gScene.Sample(gLinear, i.uv + float2( t.x,  t.y));
    return c / 16.0;
}

// Interleaved-gradient-noise (Jimenez) — high-quality, cheap spatial dither base.
float ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Per-channel triangular-PDF (TPDF) dither in [-1,+1] from two offset IGN samples,
// added just before the 8-bit (R8G8B8A8) write so smooth gradients don't quantize
// into visible bands — the eye averages the sub-LSB noise into a smooth ramp. TPDF
// (not flat) is the textbook de-banding choice; per-channel decorrelates RGB.
// Spatial only (no per-frame term) so a still frame doesn't shimmer.
float3 dither_rgb(float2 px) {
    float3 r0 = float3(ign(px),         ign(px + 19.19), ign(px + 47.47));
    float3 r1 = float3(ign(px + 3.33),  ign(px + 71.70), ign(px + 113.1));
    return r0 + r1 - 1.0;
}

float4 PSTonemap(VSOut i) : SV_Target {
    float3 hdr = gScene.Sample(gLinear, i.uv).rgb;

    // Passthrough mode (the standard shader already display-encoded).
    [branch] if ((gFlags & 2) == 0) return float4(hdr, 1.0);

    // 틸트시프트 DoF 합성: CoC로 선명(풀해상도)↔블러(하프해상도) 보간. 낮은 CoC는
    // 풀해상도를 유지해 "선명 밴드"가 하프해상도 리샘플로 물러지지 않게 게이트.
    [branch] if (gFlags & 8) {
        float4 dof = gDof.Sample(gLinear, i.uv);
        hdr = lerp(hdr, dof.rgb, smoothstep(0.02, 0.30, dof.a));
    }

    hdr += gBloom.Sample(gLinear, i.uv).rgb * gBloomIntensity;

    float3 c = hdr * gExposure;
    float3 outc = (gFlags & 1) ? agx_tonemap(c) : srgb_encode(c);
    // TPDF dither: ±1.5 LSB of the 10-bit backbuffer (잔여 밴딩 정리 — 주 해결은
    // R10G10B10A2 백버퍼로 채널당 1024단계). 1023 = 10-bit max code.
    outc += dither_rgb(i.pos.xy) * (1.5 / 1023.0);
    return float4(outc, 1.0);
}
