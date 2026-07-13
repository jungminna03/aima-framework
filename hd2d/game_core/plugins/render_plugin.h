#pragma once

#include "arimu/App.hpp"

#include <cmath>

namespace hd2d {

// All drawing, in the Render phase (read-only of game state). Order: begin the
// forward pass (per-frame constants) -> low-poly meshes -> billboard sprites.
// The command list comes from the RenderContext resource the game loop refreshes.
struct RenderPlugin {
    static void Build(Arimu::App& app);
};

// 정규화 시간대 [0,1) = **게임 시계의 하루 분율(자정=0, 시각 = t×24 — clock_hhmm과 동일
// 규약)**. GameTimeSystem이 매 프레임 갱신; HUD의 F2 시간 바가 읽어서 표시하고,
// set_time_of_day로 특정 시각으로 점프시킨다. (2026-07-10 규약 통일: 구 비주얼 규약
// "0=새벽06시"는 시계 표기와 6시간 어긋나 표시 14:00에 밤이 렌더됐다.)
extern float g_tod;

// 낮밤 비주얼 커브(시계 시각 기준): t → 태양 to_light 방향 + 월드 앰비언트 / 하늘색.
// 밤 = **정확히 20:00 도달, 05:00까지 유지**(사용자 2026-07-10 "밤은 정확히 20시 이후").
// render/sound/HD2D_CLOCKTEST가 공유하는 단일 소스(정의 render_plugin.cpp).
void day_night_lighting(float t, float out_to_light[3], float out_ambient[3]);
void day_night_sky(float t, float out[3]);
// 이산 밤 판정(마을 음악 등): 20:00~06:00. 통금(InnExitGuard 19:00~)은 게임플레이 고유 창.
inline bool tod_is_night(float t) {
    const float h = (t - std::floor(t)) * 24.0f;
    return h >= 20.0f || h < 6.0f;
}
void set_time_of_day(float t01);
// GameTimeSystem이 소비: set_time_of_day로 요청된 점프 시각(없으면 -1)을 반환하고 채널을
// 리셋한다. 게임 시계(GameTime)가 시간의 단일 권위이므로 비주얼 자체 전진은 폐지됐고,
// F2 시간 바 등의 점프는 GameTimeSystem이 이 채널로 흡수해 accumulated_seconds에 반영한다.
float consume_time_of_day_jump();

} // namespace hd2d
