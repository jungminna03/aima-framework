#pragma once

// ============================================================================
// sdlgpu/live_scene.h — the per-frame bridge from the game's ECS to the
// SDL_GPU renderer (Phase-2 rung 1 & 2: render the ACTUAL game).
//
// The game's render systems are written against raw ID3D12GraphicsCommandList*
// and early-return on the sdlgpu backend (rc->gpu_cmd is null). Rather than
// fight that DX12-coupled path, the sdlgpu backend adds a SEPARATE render
// system (game/plugins/render_plugin.cpp, gated on HD2D_RENDERER_SDLGPU) that
// reads the SAME ECS components (MeshRenderer / WorldMatrix / BillboardSprite /
// MapLight / OrbitCamera) and fills this plain-data LiveScene each frame. It
// hands the LiveScene to the device (Dx12Device::set_live_scene), and the
// device's begin_frame renders it through GeometryPass — so what's on screen is
// the live game world from the live camera, not geometry_pass's independent glb.
//
// Everything here is plain data using only types both the host (hd2d_core) and
// the game module (hd2d_game) already share (Float3 / XMFLOAT4X4 / raw const
// pointers into stable ECS storage / std::string). No SDL or DX12 types leak.
// ============================================================================

#include "core/math_compat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hd2d {

// One world mesh to draw. The CPU geometry lives in the ECS MeshRenderer
// component (LoadedPrimitive::cpu), whose vectors are stable for the entity's
// lifetime — we pass POINTERS to them so the device can cache GPU buffers keyed
// by the positions pointer (uploaded once, reused every frame).
struct LiveMesh {
    const std::vector<Float3>*    positions = nullptr;  // local engine space
    const std::vector<uint32_t>*  indices   = nullptr;
    math::Mat4x4                  model;                // world matrix (row-vector)
    float                          color[3] = {0.8f, 0.8f, 0.8f};
    // Camera-occlusion fade (OcclusionState): <1 => drawn translucent AFTER the
    // billboards so the player shows through. Shared game<->host layout — a
    // change here needs a FULL rebuild + restart (see CLAUDE.md live_scene note).
    float                          alpha = 1.0f;
};

// One camera-facing textured billboard (a character / prop sprite). The sheet
// pixels are loaded by path (the device caches the GPU texture by path). uv_*
// select the pose column (+ optional horizontal mirror via a negative uv_scale[0]).
// scale_x/scale_y are the paper-animation squash&stretch (non-uniform, feet-planted).
// Shared game<->host layout — a change here needs a FULL rebuild + restart
// (see CLAUDE.md live_scene note).
struct LiveBillboard {
    std::string  sheet_path;             // assets/sprite/<id>.png
    Float3       position;               // feet on the ground (quad y in [0,1])
    float        yaw_deg = 0.0f;         // rotate the quad to face the camera
    float        world_height = 1.0f;    // frame_px / pixels_per_unit
    float        scale_x = 1.0f;         // paper squash&stretch: horizontal
    float        scale_y = 1.0f;         // paper squash&stretch: vertical (feet-planted)
    float        uv_offset[2] = {0, 0};
    float        uv_scale[2] = {1, 1};
    float        tint[3] = {1, 1, 1};
};

// Everything needed to render one frame of the live game world. Refilled by the
// game's sdlgpu render system each frame; consumed by the device's begin_frame.
struct LiveScene {
    bool   valid = false;          // false => the device falls back to its own glb load
    // 맵 스왑 세대(2026-07-03): respawn_map이 증가시킨다. GeometryPass는 mesh_cache_가
    // CPU positions 포인터를 키로 쓰므로, 새 맵이 해제된 옛 주소를 재사용하면 옛 맵
    // 조각이 그대로 그려졌다("이동할 때마다 맵이 이상함"). 세대가 바뀌면 캐시 전체 플러시.
    // clear()는 건드리지 않는다(단조 증가).
    int    map_generation = 0;

    // Camera (already composed by the live OrbitCamera).
    math::Mat4x4 view;
    math::Mat4x4 proj;           // built with the frame's true aspect
    Float3         cam_pos;

    // Lighting (first directional MapLight, else the RenderSettings debug sun).
    float light_dir[3] = {0.4f, 0.8f, -0.45f};   // direction TO the light, engine space
    float ambient[3]   = {0.18f, 0.20f, 0.26f};

    // Sun shadow framing (mirrors the DX12 ShadowPassSystem). The game fills the
    // sun's ortho view*proj + the world bounds it was fit to so the device can
    // render a shadow map from the sun's POV and sample it in the geometry pass.
    bool           shadow_active = false;        // false => geometry pass skips shadows
    math::Mat4x4 sun_view_proj;                // light-space transform (row-vector)
    Float3         bounds_min{-15, -1, -15};     // map bounds (sun frustum fit)
    Float3         bounds_max{ 15,  8,  15};

    std::vector<LiveMesh>      meshes;
    std::vector<LiveBillboard> billboards;

    void clear() {
        valid = false;
        shadow_active = false;
        meshes.clear();
        billboards.clear();
    }
};

} // namespace hd2d
