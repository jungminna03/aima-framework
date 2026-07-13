#pragma once

// ============================================================================
// dof_math.h — 틸트시프트 DoF CoC 공식의 C++ 미러 (스펙 2026-07-02).
// shaders/post.hlsl PSDofCoc와 1:1 — 어느 쪽을 고치든 둘 다 고칠 것.
// HD2D_DOFTEST(player_plugin)가 자기검증하고, CameraOrbitSystem(player_plugin)이
// dof_strength_for로 상태별 강도를 이징한다. 순수 함수 — 월드/렌더러 불필요.
// ============================================================================

#include <algorithm>
#include <cmath>

namespace hd2d {

// 상태별 강도(브레인스토밍 확정 2026-07-02: 항상 켜고 강도만 상황별).
inline constexpr float kDofExplore  = 0.35f;  // 낮은 백뷰 탐험 — 은은하게
inline constexpr float kDofCombat   = 0.8f;   // 부감 24° 전투 — 디오라마 강조
inline constexpr float kDofInterior = 0.5f;   // 인테리어 근접 — 중간

struct DofParams {
    float focus_dist;     // 카메라→초점(파티) 거리 m
    float focus_range;    // 완전 선명 깊이 반폭 m
    float blur_range;     // 선명→최대 블러 전이 폭 m
    float strength;       // [0,1] 최종 스케일
    float band_center;    // 선명 밴드 중심 (uv.y, 0=화면 상단)
    float band_half;      // 밴드 반폭
    float band_feather;   // 밴드 페더 폭
    float protect_range;  // 밴드 항의 깊이 보호 반경 m
};

inline float dof_smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 비선형 깊이버퍼 값 d[0,1] → 뷰공간 선형 거리(m). 표준 Z(clear=1=far).
inline float dof_linearize(float d, float cam_near, float cam_far) {
    return cam_near * cam_far / (cam_far - d * (cam_far - cam_near));
}

// 하이브리드 CoC (스펙 §CoC 공식): 깊이 항 ∨ (화면 밴드 × 깊이 보호).
// z = 선형 깊이 m, uv_y = 화면 세로 [0,1] (0=상단). 반환 [0,1].
// 밴드 항은 화면 위/아래 "미니어처 흐림", protect는 초점 깊이의 유닛이 화면
// 가장자리에 있어도 안 흐려지게 밴드 항만 죽인다(깊이 항은 그대로).
inline float dof_coc(float z, float uv_y, const DofParams& p) {
    const float dz = std::fabs(z - p.focus_dist);
    const float coc_depth = std::clamp((dz - p.focus_range) / p.blur_range, 0.0f, 1.0f);
    const float band = dof_smoothstep(p.band_half, p.band_half + p.band_feather,
                                      std::fabs(uv_y - p.band_center));
    const float protect = std::clamp(dz / p.protect_range, 0.0f, 1.0f);
    return std::max(coc_depth, band * protect) * p.strength;
}

// 상태별 강도: 탐험↔전투는 combat_blend, 인테리어는 별도 블렌드가 덮는다.
inline float dof_strength_for(float combat_blend, float interior_blend) {
    const float base = kDofExplore + (kDofCombat - kDofExplore) * combat_blend;
    return base + (kDofInterior - base) * interior_blend;
}

} // namespace hd2d
