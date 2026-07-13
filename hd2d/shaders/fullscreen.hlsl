// Placeholder shader for the HD-2D pipeline (Phase 0).
// Day-1 hot-reload watches this directory; the DXC compile + PSO rebuild path
// is wired in Phase 0 follow-up. For now editing this file logs a [hot-reload]
// event in the debug console / ImGui panel.

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle generated from the vertex id (no vertex buffer needed).
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    return float4(i.uv, 0.5, 1.0);
}
