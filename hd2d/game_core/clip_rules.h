#pragma once

// 유닛별 조건부 클립 오버라이드 (2026-07-03) — interact-script + clip-rules 설계 Task 3.
// docs/superpowers/specs/2026-07-03-interact-script-and-clip-rules-design.md 4절.
//
// SpriteAnimationSystem의 기본 우선순위(전투 액션 > Demeanor 가드 > 이동 걷기 > idle)는
// 그대로 두고, 그 앞에서 유닛별 FactCond 규칙표를 top-to-bottom으로 평가해 첫 매치의
// AnimKey로 "덮어쓸" 수 있게 하는 순수 데이터 + 해석 헬퍼. 테이블이 없거나 매치가 없으면
// AnimKey::Count를 반환 — 호출부는 이를 "오버라이드 없음"으로 해석해 기존 로직을 그대로 쓴다.

#include "game/components.h"     // AnimKey
#include "game/facts.h"          // Fact / Ctx / FactCond / Arimu::gate_met_until

#include <cstring>
#include <iterator>

namespace hd2d {

// 규칙 하나 = 조건 게이트(전부 통과해야 채택) + 채택 시 쓸 AnimKey. conds의 빈 슬롯은
// interact_script.h와 동일한 idiom({Fact::None, nullptr, 0} = 종료 센티넬, 이하 전부 통과).
struct ClipRule { FactCond conds[4]; AnimKey key; };

struct UnitClipRules { const char* unit; const ClipRule* rules; int n; };

// smith 샘플 규칙: 재방문(트리거 t_talk_smith 완주 ≥1 — 구 TalkedOf 은퇴, 2026-07-05)이면
// Guard(기존 이미 쓰는 대기/자세 포즈)로 덮어쓴다 — 새 AnimKey를 만들지 않고 이미 있는
// enumerator를 재사용한 무해한 시연용 규칙.
inline constexpr ClipRule kSmithClipRules[] = {
    {{{Fact::TriggerCountOf, "t_talk_smith", 1}, {Fact::None, nullptr, 0}, {Fact::None, nullptr, 0}, {Fact::None, nullptr, 0}},
        AnimKey::Guard},
};

inline constexpr UnitClipRules kUnitClipRules[] = {
    {"smith", kSmithClipRules, static_cast<int>(std::size(kSmithClipRules))},
};
inline constexpr int kUnitClipRulesCount = static_cast<int>(std::size(kUnitClipRules));

inline const UnitClipRules* find_clip_rules(const char* unit) {
    if (!unit) return nullptr;
    for (const UnitClipRules& t : kUnitClipRules)
        if (std::strcmp(t.unit, unit) == 0) return &t;
    return nullptr;
}

// 규칙표를 위→아래로 훑어 conds[4]를 전부 만족하는 첫 규칙의 key를 반환(first-match-wins).
// 매치가 없으면 AnimKey::Count = "오버라이드 없음"(호출부는 기존 우선순위 결과를 그대로 쓴다).
inline AnimKey resolve_clip_key(const UnitClipRules& table, const Ctx& ctx) {
    for (int i = 0; i < table.n; ++i) {
        const ClipRule& rule = table.rules[i];
        if (Arimu::gate_met_until(rule.conds, ctx, Fact::None)) return rule.key;
    }
    return AnimKey::Count;
}

} // namespace hd2d
