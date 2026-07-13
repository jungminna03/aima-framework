// Depth-only shadow shader (sun shadow map). Alpha-tested so billboard
// sprites and MASK materials cast cutout silhouettes; opaque draws pass
// cutoff 0 and never discard.

cbuffer Frame : register(b0) {
    row_major float4x4 gSunViewProj;
};

cbuffer Draw : register(b1) {
    row_major float4x4 gModel;
    float  gAlphaCutoff;
    float3 _pad;
    float2 gUvOffset;
    float2 gUvScale;
};

Texture2D    gTexBase : register(t0);
SamplerState gSamp    : register(s0);

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
};

struct VSOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.clip = mul(mul(float4(i.pos, 1.0), gModel), gSunViewProj);
    o.uv = i.uv * gUvScale + gUvOffset;
    return o;
}

void PSMain(VSOut i) {
    if (gAlphaCutoff > 0.0) {
        if (gTexBase.Sample(gSamp, i.uv).a < gAlphaCutoff) discard;
    }
}
