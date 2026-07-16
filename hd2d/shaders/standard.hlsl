// HD-2D standard surface shader — the ONE shader everything goes through.
// Low-poly glTF meshes AND billboard pixel sprites are drawn with this exact
// pipeline; a sprite is just an alpha-tested textured quad. Compiled at runtime
// with D3DCompile (SM 5.1) and hot-reloadable.
//
// Shading: glTF-spec Cook-Torrance (GGX + Schlick Fresnel + Smith visibility)
// with up to 16 punctual lights in radiometric units — the same model and
// units Blender's Eevee uses, so a .glb map looks like its Blender viewport.
// Output is scene-linear HDR; the post chain applies exposure + AgX.

#define MAX_LIGHTS 16
static const float PI = 3.14159265359;

// Lights are packed as parallel float4 arrays (SoA): FXC's cbuffer layout for
// float4[] is unambiguous (1 row per element), unlike struct arrays.
cbuffer Frame : register(b0) {
    row_major float4x4 gViewProj;
    float3 gCamPos;   float gLightCount;
    float3 gAmbient;  float _pad0;            // world ambient radiance
    row_major float4x4 gSunViewProj;          // shadow-map transform
    float4 gShadowParams;                     // x texel, y bias, z normal-offset, w sun index (-1 off)
    float4 gLightPosRange[MAX_LIGHTS];        // xyz pos, w range (0 = unbounded)
    float4 gLightDirType[MAX_LIGHTS];         // xyz dir, w type (0 dir/1 point/2 spot)
    float4 gLightColorInner[MAX_LIGHTS];      // rgb color*W, w inner cone cos
    float4 gLightOuter[MAX_LIGHTS];           // x outer cone cos
};

struct Light {
    float3 pos;     float range;
    float3 dir;     float type;
    float3 colorW;  float innerCos;
    float  outerCos;
};

Light fetch_light(int i) {
    Light L;
    L.pos = gLightPosRange[i].xyz;     L.range = gLightPosRange[i].w;
    L.dir = gLightDirType[i].xyz;      L.type = gLightDirType[i].w;
    L.colorW = gLightColorInner[i].rgb; L.innerCos = gLightColorInner[i].w;
    L.outerCos = gLightOuter[i].x;
    return L;
}

cbuffer Draw : register(b1) {
    row_major float4x4 gModel;
    row_major float4x4 gNormalMat;
    float4 gBaseColor;
    float  gMetallic;
    float  gRoughness;
    float  gAlphaCutoff;  // sprites/MASK: alpha test threshold; 0 = opaque
    uint   gFlags;        // bit0 nearest, bit1 clamp, bit2 has normal map, bit3 unlit(sky)
    float2 gUvOffset;     // sprite frame/direction sub-rect select
    float2 gUvScale;
    float3 gEmissive;     // emissive factor * strength (linear)
    float  _pad1;
};

Texture2D gTexBase     : register(t0);
Texture2D gTexMR       : register(t1);   // G = roughness, B = metallic (glTF)
Texture2D gTexNormal   : register(t2);
Texture2D gTexEmissive : register(t3);
Texture2D gShadowMap   : register(t4);

SamplerState gSampLinearWrap  : register(s0);
SamplerState gSampLinearClamp : register(s1);
SamplerState gSampPointWrap   : register(s2);
SamplerState gSampPointClamp  : register(s3);
SamplerComparisonState gSampShadow : register(s4);

// PCF 3x3 sun shadow factor (1 = fully lit). Normal-offset + depth bias fight
// acne on the low-poly slopes.
float shadow_factor(float3 world_pos, float3 N, float ndotl) {
    if (gShadowParams.w < 0.0) return 1.0;
    // Slope-scaled normal-offset + depth bias. The day/night sun sweeps to grazing
    // angles where a large flat ground self-shadows (regular acne stripes) — at low
    // ndotl the depth across one shadow texel is huge, so a fixed bias can't cover it.
    // Both offsets grow as the sun grazes (ndotl -> 0); at noon (ndotl=1) slope=1 = old.
    float slope = clamp(tan(acos(saturate(ndotl))), 1.0, 10.0);
    float3 wp = world_pos + N * (gShadowParams.z * slope);
    float4 sc = mul(float4(wp, 1.0), gSunViewProj);
    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float ref = sc.z - gShadowParams.y * slope;
    // 5x5 PCF with a widened tap step → soft penumbra. (작은 텍셀이라 3x3은 경계가
    // 1-텍셀짜리 딱딱한 선이 됨; 탭을 넓혀 부드럽게.) kSoft = penumbra 폭(클수록 부드러움).
    const float kSoft = 2.5;
    float sum = 0.0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx)
            sum += gShadowMap.SampleCmpLevelZero(
                gSampShadow, uv + float2(dx, dy) * (gShadowParams.x * kSoft), ref);
    return sum / 25.0;
}

float4 sample_map(Texture2D t, float2 uv) {
    [branch] if ((gFlags & 1) == 0) {
        [branch] if ((gFlags & 2) == 0) return t.Sample(gSampLinearWrap, uv);
        return t.Sample(gSampLinearClamp, uv);
    }
    [branch] if ((gFlags & 2) == 0) return t.Sample(gSampPointWrap, uv);
    return t.Sample(gSampPointClamp, uv);
}

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
};

struct VSOut {
    float4 clip    : SV_Position;
    float3 worldPos: TEXCOORD0;
    float3 normal  : TEXCOORD1;
    float2 uv      : TEXCOORD2;
    float4 color   : TEXCOORD3;
    float4 tangent : TEXCOORD4;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gModel);
    o.worldPos = wp.xyz;
    o.clip = mul(wp, gViewProj);
    o.normal = mul(i.normal, (float3x3)gNormalMat);
    o.tangent = float4(mul(i.tangent.xyz, (float3x3)gModel), i.tangent.w);
    o.uv = i.uv * gUvScale + gUvOffset;
    o.color = i.color;
    return o;
}

// ---------------------------------------------------------------------------
// glTF metallic-roughness BRDF (what Eevee evaluates for a Principled BSDF).
// ---------------------------------------------------------------------------
float d_ggx(float ndoth, float a) {
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

// Hammon's approximation of height-correlated Smith visibility.
float v_smith(float ndotl, float ndotv, float a) {
    return 0.5 / max(lerp(2.0 * ndotl * ndotv, ndotl + ndotv, a), 1e-5);
}

float3 f_schlick(float3 f0, float vdoth) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - vdoth), 5.0);
}

// One light's outgoing radiance contribution at the shaded point.
float3 shade_light(Light L, float3 P, float3 N, float3 V,
                   float3 diffuse_color, float3 f0, float a) {
    float3 to_light;
    float atten = 1.0;

    [branch] if (L.type < 0.5) {                  // directional: colorW = W/m^2
        to_light = -L.dir;
    } else {
        float3 dvec = L.pos - P;
        float dist2 = max(dot(dvec, dvec), 1e-4);
        float dist = sqrt(dist2);
        to_light = dvec / dist;
        atten = 1.0 / (4.0 * PI * dist2);         // E = Phi / (4 pi r^2)
        [branch] if (L.range > 0.0) {             // KHR range windowing
            float w = saturate(1.0 - pow(dist / L.range, 4.0));
            atten *= w * w;
        }
        [branch] if (L.type > 1.5) {              // spot cone falloff
            float cd = dot(L.dir, -to_light);
            float t = saturate((cd - L.outerCos) /
                               max(L.innerCos - L.outerCos, 1e-4));
            atten *= t * t;
        }
    }

    float ndotl = saturate(dot(N, to_light));
    if (ndotl <= 0.0) return 0.0;

    float3 H = normalize(to_light + V);
    float ndotv = saturate(dot(N, V)) + 1e-5;
    float ndoth = saturate(dot(N, H));
    float vdoth = saturate(dot(V, H));

    float3 F = f_schlick(f0, vdoth);
    float3 spec = d_ggx(ndoth, a) * v_smith(ndotl, ndotv, a) * F;
    float3 diff = diffuse_color / PI;

    return (diff + spec) * L.colorW * atten * ndotl;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 tex = sample_map(gTexBase, i.uv);
    float4 surf = gBaseColor * tex * i.color;

    // Alpha test (no blend) -> billboards get free depth sorting.
    if (surf.a < gAlphaCutoff) discard;

    // bit3(8) = unlit (3D skybox): no lights/shadow/ambient — the sky IS the
    // light source. Emissive still adds (glowing sun disc etc.).
    [branch] if (gFlags & 8)
        return float4(surf.rgb + gEmissive * sample_map(gTexEmissive, i.uv).rgb, surf.a);

    float3 N = normalize(i.normal);
    [branch] if (gFlags & 4) {
        float3 T = normalize(i.tangent.xyz);
        float3 B = cross(N, T) * i.tangent.w;
        float3 tn = sample_map(gTexNormal, i.uv).xyz * 2.0 - 1.0;
        N = normalize(tn.x * T + tn.y * B + tn.z * N);
    }
    // Double-sided/cull-none: flip the normal toward the viewer so backfaces
    // (flags, cloth) light correctly instead of going black.
    float3 V = normalize(gCamPos - i.worldPos);
    if (dot(N, V) < 0.0) N = -N;

    float4 mr = sample_map(gTexMR, i.uv);
    float roughness = clamp(gRoughness * mr.g, 0.045, 1.0);
    float metallic = saturate(gMetallic * mr.b);
    float a = roughness * roughness;

    float3 diffuse_color = surf.rgb * (1.0 - metallic);
    float3 f0 = lerp(0.04, surf.rgb, metallic);

    float3 lit = 0.0;
    const int count = (int)gLightCount;
    const int sun_index = (int)gShadowParams.w;
    [loop] for (int li = 0; li < count; ++li) {
        Light L = fetch_light(li);
        float3 c = shade_light(L, i.worldPos, N, V, diffuse_color, f0, a);
        if (li == sun_index) {
            float sndl = saturate(dot(N, -L.dir));   // sun is directional: to-light = -dir
            c *= shadow_factor(i.worldPos, N, sndl);
        }
        lit += c;
    }

    // Constant-environment ambient: diffuse exactly, specular approximated.
    lit += diffuse_color * gAmbient + f0 * gAmbient * 0.5;

    lit += gEmissive * sample_map(gTexEmissive, i.uv).rgb;

    // Alpha = surface alpha so the translucent-occluder pass (camera-occlusion
    // fade) can blend; opaque meshes have surf.a≈1 so the opaque pass is unchanged.
    return float4(lit, surf.a);   // scene-linear HDR; post applies the transform
}
