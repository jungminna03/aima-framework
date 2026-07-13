#pragma once

#include "renderer/gpu_resources.h"

#include <string>

namespace hd2d {

constexpr int kSheetRows = 1;  // 페이퍼 스프라이트(2026-07-08): 시트는 1행 — 포즈 = 열.

// A loaded single-row sprite sheet (페이퍼 애니메이션).
//   sheet = (frame_px * frame_count) wide  x  frame_px tall  (1 row of square cells)
//   캐릭터: 4열 = 방향 포즈(0정면/1앞대각/2뒷대각/3뒷면), 좌우는 런타임 UV 플립.
//   프롭: 1열 정지 이미지. (구 8행=방향 플립북 계약 폐지)
struct SpriteSheet {
    rhi::GpuTexture texture;   // host Dx12ResourceTable handle (PAL render keystone)
    int frame_px = 0;
    int frame_count = 0;   // 열 개수(캐릭터=포즈 수, 프롭=1)
    bool valid = false;
};

// Load a sheet, auto-detecting frame_px from the single-row contract
// (frame_px = height; frame_count = width / height). The loader errors loudly on a
// malformed file — width not a multiple of height (non-square cells) — returning a
// sheet with valid=false (the sprite is skipped).
bool load_sprite_sheet(Dx12Device& dev, Dx12ResourceTable& table, const std::string& path, SpriteSheet& out);

} // namespace hd2d
