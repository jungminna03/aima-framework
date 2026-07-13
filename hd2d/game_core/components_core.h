#pragma once

// ----------------------------------------------------------------------------
// components_core.h — 렌더 기초 컴포넌트 (프레임워크 game_core 승격, 2026-07-13).
// game/components.h 에서 잘라낸 Transform / MeshRenderer / MapLight /
// BillboardSprite. 게임 전용 컴포넌트(WorldMatrix/MapEntity/Player/AnimKey/quest/
// party 등)는 game/components.h 에 잔류하며, 그 파일이 이 헤더를 include 한다.
//
// ⚠️ 레이아웃 불변 계약: 이 구조체들의 필드 순서/타입은 승격 전과 byte-identical —
// GameStateVersion 호환성을 위해 변경 금지.
// ----------------------------------------------------------------------------

#include "assets/gltf_loader.h"
#include "assets/sprite_sheet.h"
#include "core/math_compat.h"

#include <string>
#include <vector>

namespace hd2d {

// World transform. For billboards, yaw_deg is the character's FACING direction
// (which way it "looks") — the 8-direction row is chosen from this vs the camera.
struct Transform {
    Float3 position;
    float yaw_deg = 0.0f;
    float scale = 1.0f;
};

// A low-poly glTF mesh node (one or more primitives, each with a material index
// into the MapScene resource + a CPU copy for physics/cloth).
struct MeshRenderer {
    std::vector<LoadedPrimitive> prims;
};

// A punctual light authored in Blender (position/direction come from the
// entity's WorldMatrix).
struct MapLight {
    LoadedLight light;
};

// A pixel-art billboard. Authoring contract = path; everything else is filled on
// load + the paper-animation system. 페이퍼 애니(2026-07-08): 방향별 정지 포즈 4종
// (0정면/1앞대각/2뒷대각/3뒷면) + 좌우 플립으로 8방향을 커버하고, 이동/공격/피격/
// 죽음은 수학 트랜스폼(bob/off/scale)으로 표현 — 프레임 넘김(플립북) 없음.
struct BillboardSprite {
    std::string path;       // sheet image
    float fps = 8.0f;       // prop whole-sheet loop rate (unused by characters now)

    // runtime state (filled by the load + paper-animation systems)
    SpriteSheet sheet;
    float anim_time = 0.0f; // per-entity clock (idle breathe / hit shake)
    int cur_frame = 0;      // sheet COLUMN = pose (0정면 1앞대각 2뒷대각 3뒷면); prop=0
    int cur_dir = 0;        // sheet ROW — always 0 (single-row sheet); kept for uv math
    int cur_anim_key = -1;  // (vestigial)
    bool load_attempted = false;

    // paper-animation transforms — visual only, never touch physics. Filled each
    // frame by PaperAnimationSystem; composed into LiveBillboard by render_plugin.
    bool  flip_x = false;   // mirror U (왼쪽 방향 = 오른쪽 포즈 반전)
    float bob_y = 0.0f;     // 수직 홉 / 죽음 가라앉기 오프셋(m)
    float off_x = 0.0f;     // 수평 오프셋: 런지/흔들림(m)
    float off_z = 0.0f;
    float scale_x = 1.0f;   // 비균일 스케일(스쿼시&스트레치): 가로
    float scale_y = 1.0f;   // 세로(발밑 고정)
    float hop_dist = 0.0f;  // 누적 수평 이동거리(홉 위상)
    float death_t = 0.0f;   // 죽음 진행[0,1]
};

} // namespace hd2d
