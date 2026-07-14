#pragma once

#include "arimu/App.hpp"

namespace hd2d {

// FxSystem 등록 플러그인. Logic 스케줄에 FxSystem 하나를 단다 —
// EffectFx 진행(프레임 인덱스 + 모션/틴트 커브 → BillboardSprite 채널) + 원샷 소멸.
//
// ⚠️ 등록 순서(fx_plugin.cpp 주석 참조): CombatTintSystem(게임측, bb.tint 리셋→주입)
// 과 PaperAnimationSystem(sprite_plugin, scale/off/bob/cur_frame 리셋→주입) *이후*·
// 렌더(Render 페이즈) 이전에 실행되어야 커브가 덮어써진다. game_api.cpp 는 FxPlugin 을
// SpritePlugin 뒤에 AddPlugin 하여 이 순서를 보장한다.
struct FxPlugin {
    static void Build(Arimu::App& app);
};

} // namespace hd2d
