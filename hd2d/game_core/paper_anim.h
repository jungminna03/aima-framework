#pragma once
// 페이퍼 스프라이트 애니메이션 — 순수 수학 (플립북 대체).
// 설계: docs/superpowers/specs/2026-07-08-paper-sprite-animation-design.md
//
// 캐릭터는 방향별 정지 스프라이트(4포즈) 한 장만 갖고, 이동/공격/피격/죽음은
// 트랜스폼(위치·비균일 스케일)을 매 프레임 수학으로 계산해 표현한다.
// 이 헤더의 함수는 PaperAnimationSystem(런타임)과 HD2D_PAPERTEST(자기검증)가
// 공유한다 — 월드/렌더러 의존 없음(헤더-온리, 순수).

#include <cmath>
#include <cstdio>

namespace paper {

// ---- 튜닝 상수 (미터; 안착 후 게임코드 핫리로드로 조정 가능) ----
constexpr float kStride = 1.667f; // 홉 1회 파장(이동 거리 m) — 걷기 5m/s에서 ≈3홉/초(사용자 2026-07-08)
constexpr float kHopH   = 0.10f;  // 걷기 홉 높이 m
constexpr float kSquash = 0.11f;  // 스쿼시/스트레치 강도(정점 +, 착지 −)
constexpr float kPi     = 3.14159265358979323846f;

// ---- 4포즈 시트 열 인덱스 ----
enum Pose { kFront = 0, kFrontDiag = 1, kBackDiag = 2, kBack = 3, kPoseCount = 4 };

struct PoseFlip { int pose; bool flip; };

// 카메라 상대 facing(도) → (포즈 열, 좌우 플립).
// rel_deg = yaw_to_cam − unit_yaw. 0=정면(카메라 향함), ±180=뒷면.
// 가장 가까운 포즈: 중심 0/45/135/180, 90°(동점)은 앞대각으로.
// 부호(<0 = 왼쪽 절반)로 플립 — 아트는 오른쪽 향함 기준. 정면/뒷면은 대칭이라 무플립.
// (부호가 화면 좌/우 어느 쪽인지는 런타임에서 알려진 케이스로 확정; kFlipSign로 뒤집기 용이.)
constexpr float kFlipSign = -1.0f; // rel*kFlipSign < 0 이면 플립. 인터랙티브 검증에서 좌우 반대라 −1로 확정(사용자 2026-07-08).

inline PoseFlip pose_flip_of(float rel_deg) {
    float r = rel_deg;
    while (r > 180.0f)  r -= 360.0f;
    while (r < -180.0f) r += 360.0f;
    bool flip = (r * kFlipSign) < 0.0f;
    const float a = std::fabs(r);        // 0..180
    int pose;
    if (a < 22.5f)        pose = kFront;      // 0° 근처
    else if (a <= 90.0f)  pose = kFrontDiag;  // 45°, 90°(동점) 포함
    else if (a < 157.5f)  pose = kBackDiag;   // 135° 근처
    else                  pose = kBack;       // 180° 근처
    if (pose == kFront || pose == kBack) flip = false;  // 대칭 포즈
    return { pose, flip };
}

// 걷기 홉: 이동거리(누적 수평 m) → 수직 오프셋(m). 스트라이드당 1홉, 발밑에서 정점까지.
inline float bob_of(float hop_dist) {
    const float p = hop_dist / kStride;               // 파장 = kStride
    return kHopH * std::fabs(std::sin(kPi * p));      // 0..kHopH
}

// 스쿼시&스트레치: 현재 홉 높이(0..kHopH) → 비균일 스케일.
// 정점(위)=세로 늘어남/가로 좁아짐, 착지(0)=세로 짜부/가로 넓어짐. 발밑 기준 스케일.
struct Squash { float sx; float sy; };
inline Squash squash_of(float bob_y) {
    const float t = (kHopH > 0.0f) ? (bob_y / kHopH) : 0.0f;  // 0=지면 .. 1=정점
    const float stretch = kSquash * (2.0f * t - 1.0f);        // −kSquash..+kSquash
    return { 1.0f - stretch, 1.0f + stretch };
}

// 아이들 미세 숨쉬기: 시간 → 세로 스케일(≈1).
inline float idle_breathe(float t) {
    return 1.0f + 0.03f * std::sin(t * 2.0f);
}

// 공격 런지: 액션 위상[0,1] → 전진 거리(m). 0→0, 0.5→최대, 1→0.
constexpr float kLungeD = 0.30f;
inline float lunge_of(float phase01) {
    return kLungeD * std::sin(kPi * phase01);
}

// 피격 흔들림: 세기[0,1] + 시간 → 수평 오프셋(m). 감쇠는 호출측(norm)이 줌.
constexpr float kShake = 0.06f;
inline float shake_of(float norm, float t) {
    return kShake * norm * std::sin(t * 40.0f);
}

// 죽음: 진행[0,1] → 가라앉기(음수 y m) + 납작 스케일.
constexpr float kSinkD = 0.30f;
inline float death_sink_of(float d) { return -kSinkD * d; }
inline Squash death_squash_of(float d) { return { 1.0f + 0.3f * d, 1.0f - 0.8f * d }; }

}  // namespace paper

namespace hd2d {

// HD2D_PAPERTEST — 순수 자기검증(창/월드 불필요, 즉시 종료; compass_math 패턴).
// ① facing→(포즈,플립) ② 홉 위상 ③ 스쿼시 커플링.
inline int run_paper_check() {
    bool all_ok = true;
    auto check = [&](const char* name, bool ok) {
        if (!ok) all_ok = false;
        std::printf("[paper] %s %s\n", name, ok ? "ok" : "FAIL");
    };
    using namespace paper;

    // --- pose-map: 8 quantized facings (rel deg) → pose col + flip ---
    check("front  (0 -> 0, no flip)",  pose_flip_of(0.0f).pose == kFront && !pose_flip_of(0.0f).flip);
    check("fdiag  (45 -> 1)",          pose_flip_of(45.0f).pose == kFrontDiag);
    check("side   (90 -> fdiag tie)",  pose_flip_of(90.0f).pose == kFrontDiag);
    check("bdiag  (135 -> 2)",         pose_flip_of(135.0f).pose == kBackDiag);
    check("back   (180 -> 3, no flip)", pose_flip_of(180.0f).pose == kBack && !pose_flip_of(180.0f).flip);
    check("+45 flips (kFlipSign=-1)",  pose_flip_of(45.0f).pose == kFrontDiag && pose_flip_of(45.0f).flip);
    check("-45 no flip",               pose_flip_of(-45.0f).pose == kFrontDiag && !pose_flip_of(-45.0f).flip);
    check("wrap 270->-90 no flip",     pose_flip_of(270.0f).pose == kFrontDiag && !pose_flip_of(270.0f).flip);

    // --- bounce: distance-phased hop ---
    check("bob p0 = 0",                std::fabs(bob_of(0.0f)) < 1e-4f);
    check("bob half-stride = peak",    std::fabs(bob_of(kStride * 0.5f) - kHopH) < 1e-3f);
    check("bob full-stride = 0",       bob_of(kStride) < 1e-3f);
    check("bob >= 0 always",           bob_of(kStride * 0.3f) >= 0.0f && bob_of(kStride * 0.7f) >= 0.0f);

    // --- squash: ground=짜부(sy<1,sx>1), apex=늘어남(sy>1,sx<1) ---
    check("squash ground sy<1 sx>1",   squash_of(0.0f).sy < 1.0f && squash_of(0.0f).sx > 1.0f);
    check("squash apex   sy>1 sx<1",   squash_of(kHopH).sy > 1.0f && squash_of(kHopH).sx < 1.0f);

    // --- lunge / death shape ---
    check("lunge 0->0, .5->peak",      std::fabs(lunge_of(0.0f)) < 1e-4f && std::fabs(lunge_of(0.5f) - kLungeD) < 1e-3f);
    check("death sinks + flattens",    death_sink_of(1.0f) < 0.0f && death_squash_of(1.0f).sy < 1.0f);

    std::printf("[papertest] RESULT %s\n", all_ok ? "GREEN" : "RED");
    return all_ok ? 0 : 1;
}

}  // namespace hd2d
