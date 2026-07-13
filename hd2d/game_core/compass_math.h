#pragma once

// ============================================================================
// compass_math.h — 나침반 바늘 각도의 순수 계산 (사용자 2026-07-07 "바늘 나침반").
// 고정 다이얼(북=위/동=우/남=아래/서=좌) + 빨간 바늘이 월드 '북'을 가리킨다.
// 화면각 규약: 0 = 위(12시, 카메라 전방), + = 시계방향(오른쪽). HUD가
//   pos = center + (sin θ, -cos θ)·R 로 그린다 (hud_plugin CompassHudSystem —
//   은퇴한 VillageMarkerHud와 같은 카메라 전방/우측 기저).
// 월드 방위: 북 = -Z, 동 = +X, 남 = +Z, 서 = -X (yaw 0 = 카메라가 -Z를 봄 = 정북).
// 순수 함수 — 월드/렌더러 불필요. HD2D_COMPASSTEST가 자기검증(main.cpp).
// ============================================================================

#include <cmath>
#include <cstdio>

namespace hd2d {

inline constexpr float kCompassPi = 3.14159265358979323846f;

// 월드 XZ 방위 벡터 D=(dx,dz)를 카메라 yaw(도) 기준 화면각(라디안, 0=위·+=시계)으로.
// 카메라 전방 = (sin y, -cos y), 우측 = (cos y, sin y) — 마커/HUD와 동일 기저.
//   위 성분(fwd) = D·전방, 오른쪽 성분(side) = D·우측 → atan2(side, fwd).
inline float compass_screen_angle(float yaw_deg, float dx, float dz) {
    const float y = yaw_deg * (kCompassPi / 180.0f);
    const float fwd  = dx * std::sin(y) + dz * (-std::cos(y));   // 위(+)/아래(-)
    const float side = dx * std::cos(y) + dz * (std::sin(y));    // 오른쪽(+)/왼쪽(-)
    return std::atan2(side, fwd);                                // 0=위, +=시계
}

// 빨간 바늘(월드 북 = -Z)의 화면각. 정면이 북이면 0(위), 일반적으로 -yaw로 수렴.
inline float compass_north_angle(float yaw_deg) {
    return compass_screen_angle(yaw_deg, 0.0f, -1.0f);   // 북 = -Z
}

// ---------------------------------------------------------------------------
// HD2D_COMPASSTEST: 순수 자기검증(창/월드 불필요, 즉시 종료). main.cpp가 최상단에서
// 디스패치. 바늘이 카메라 회전에 맞춰 월드 북을 가리키는지 + 다이얼 방위 기저를 확인.
// ---------------------------------------------------------------------------
inline int run_compass_check() {
    bool all_ok = true;
    auto ang_close = [](float a, float b) {                 // 각도 wrap 고려한 근사비교
        float d = a - b;
        while (d >  kCompassPi) d -= 2.0f * kCompassPi;
        while (d < -kCompassPi) d += 2.0f * kCompassPi;
        return std::fabs(d) < 1e-3f;
    };
    auto check = [&](const char* name, bool ok) {
        if (!ok) all_ok = false;
        std::printf("[compass] %s %s\n", name, ok ? "ok" : "FAIL");
    };
    const float H = kCompassPi * 0.5f;
    check("north up   (yaw 0   -> 0)",    ang_close(compass_north_angle(0.0f),   0.0f));
    check("east  left (yaw 90  -> -90)",  ang_close(compass_north_angle(90.0f),  -H));
    check("south down (yaw 180 -> 180)",  ang_close(std::fabs(compass_north_angle(180.0f)), kCompassPi));
    check("west  right(yaw 270 -> +90)",  ang_close(compass_north_angle(270.0f), +H));
    check("wraps      (yaw 360 == 0)",    ang_close(compass_north_angle(360.0f), 0.0f));
    // 고정 다이얼 기저: yaw 0에서 동쪽 방위(+X)는 화면 오른쪽(+90).
    check("dial east  (yaw 0,+X-> +90)",  ang_close(compass_screen_angle(0.0f, 1.0f, 0.0f), +H));
    std::printf("[compasstest] RESULT %s\n", all_ok ? "GREEN" : "RED");
    return all_ok ? 0 : 1;
}

} // namespace hd2d
