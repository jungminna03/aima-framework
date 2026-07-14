#pragma once

// ----------------------------------------------------------------------------
// fx.h — 통합 이펙트 컴포넌트 + 스폰 API (프레임워크 game_core, 2026-07-14).
//
// EffectFx 컴포넌트를 단 엔티티는 FxSystem(fx_plugin)이 매 프레임 진행시킨다:
//   t 진행 → 프레임 인덱스 + 모션/틴트 커브 → BillboardSprite 채널.
// 미지 id 스폰은 경고 후 no-op(엔티티 미생성).
// ----------------------------------------------------------------------------

#include "game_core/components_core.h"   // Transform / BillboardSprite / Float3
#include "game_core/fx_defs.h"

#include <entt/entt.hpp>

namespace hd2d {

// 활성 이펙트 인스턴스. def=정의행, t=경과시간(초), follow=추적 대상(null=월드고정),
// base_y=스폰 기준 높이(follow 추적 시 대상 y 에 얹는 오프셋 기준).
struct EffectFx {
    const EffectDef* def   = nullptr;
    float            t     = 0.0f;
    entt::entity     follow = entt::null;
    float            base_y = 0.0f;
};

namespace fx {

// 월드 고정 이펙트. Transform + BillboardSprite + EffectFx 엔티티를 즉시 생성한다
// (registry.create()/emplace — 뷰 순회 중이 아닌 컨텍스트에서 호출할 것). 미지 id 는
// [fx] unknown 경고 후 entt::null 반환(no-op). 성공 시 [fx] spawn 로그.
entt::entity spawn(entt::registry& reg, const char* id, Float3 pos);

// 유닛 추적 이펙트(대상 Transform 를 매 프레임 따라감; 대상 소멸 시 이펙트도 소멸).
entt::entity spawn_on(entt::registry& reg, const char* id, entt::entity target);

} // namespace fx
} // namespace hd2d
