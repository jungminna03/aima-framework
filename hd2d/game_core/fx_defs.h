#pragma once

// ----------------------------------------------------------------------------
// fx_defs.h — 통합 이펙트(FX) 정의 테이블 (프레임워크 game_core, 2026-07-14).
//
// 손그림 플립북 시트 + 수학 모션 커브(페이퍼 애니 일반화)를 데이터 한 줄로 기술한다.
// designated-initializer 배열 = 단일 소스(트리거 다이얼로그 규약과 동일 스타일).
// 소스 오브 트루스 = aima-framework 리포. HD2D vendored 복사본은 rsync 반영.
//
// 아트 규약: 정사각 프레임 가로 1행 PNG(sprite_sheet 로더: frame_px=height,
// frame_count=width/height). sheet 필드 = 스프라이트명(assets/sprite/<sheet>.png).
// frames = 시트 프레임 수(작가가 아는 값 — 스폰 로그/원샷 수명 계산에 쓴다. 로더가
// 실제 시트에서 검출한 frame_count 와 어긋나면 min 으로 클램프).
// ----------------------------------------------------------------------------

namespace hd2d {

struct EffectDef {
    const char* id;        // 문자열 키(spawn 호출자가 지정: "hit"/"slash_arc"/"heal")
    const char* sheet;     // 스프라이트명 → res::sprite_path(sheet) → assets/sprite/<sheet>.png
    int   frames;          // 시트 프레임 수(작가 명시값)
    float fps;             // 프레임 재생 속도
    bool  loop;            // true=루프(수명 무한), false=원샷(수명 = frames/fps 후 소멸)

    // 모션 커브(전부 시각 전용 — 물리 무접촉, BillboardSprite 채널에 기록)
    float scale_in;        // 0→1 팝 시간(초)
    float scale_out;       // 소멸 직전 축소 시간(초; 원샷만 의미)
    float spin_dps;        // 회전 속도(deg/s) — off_x/off_z 궤도로 표현
    float rise_mps;        // 상승 속도(m/s) → bob_y
    float shake_amp;       // 흔들림 진폭(m)

    // 틴트 커브(수명 진행 t01 에 걸쳐 tint0→tint1 선형 보간). >1.0 = HDR 블룸 글로우
    // (기존 파이어볼 3.2/1.7/0.9 패턴). BillboardSprite.tint_r/g/b 에 기록.
    float tint0[3];
    float tint1[3];

    float height_m;        // 월드 높이(스폰 y 오프셋, 기본 1.0)
};

// 초기 3종. 손그림(Aseprite)이 정본; 초기 플레이스홀더는 tools/gen_fx.py 공급.
//   hit       — 피격 스파크(원샷 6f): HDR 주황 스파크가 튀며 상승/흔들림.
//   slash_arc — 베기 궤적(원샷 4f): 밝은 은청 아크, 짧고 빠르게.
//   heal      — 힐 오라(루프 8f): 초록 발광이 천천히 돌며 상승.
inline constexpr EffectDef kEffectDefs[] = {
    {   .id = "hit", .sheet = "fx_hit", .frames = 6, .fps = 20.0f, .loop = false,
        .scale_in = 0.05f, .scale_out = 0.08f, .spin_dps = 0.0f, .rise_mps = 0.5f,
        .shake_amp = 0.15f,
        .tint0 = {3.2f, 1.7f, 0.9f}, .tint1 = {1.2f, 0.6f, 0.3f}, .height_m = 0.8f },

    {   .id = "slash_arc", .sheet = "fx_slash_arc", .frames = 4, .fps = 16.0f, .loop = false,
        .scale_in = 0.02f, .scale_out = 0.05f, .spin_dps = 0.0f, .rise_mps = 0.0f,
        .shake_amp = 0.0f,
        .tint0 = {1.6f, 1.6f, 2.0f}, .tint1 = {1.0f, 1.0f, 1.2f}, .height_m = 1.0f },

    {   .id = "heal", .sheet = "fx_heal", .frames = 8, .fps = 12.0f, .loop = true,
        .scale_in = 0.20f, .scale_out = 0.0f, .spin_dps = 40.0f, .rise_mps = 0.30f,
        .shake_amp = 0.0f,
        .tint0 = {0.6f, 2.4f, 1.0f}, .tint1 = {0.8f, 1.8f, 1.0f}, .height_m = 1.0f },
};

inline constexpr int kEffectDefCount = int(sizeof(kEffectDefs) / sizeof(kEffectDefs[0]));

// id → def 조회(미지 id 는 nullptr). constexpr — 컴파일 상수 폴딩.
inline const EffectDef* find_effect_def(const char* id) {
    if (!id) return nullptr;
    for (const EffectDef& d : kEffectDefs) {
        const char* a = d.id; const char* b = id;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) return &d;   // both hit '\0'
    }
    return nullptr;
}

} // namespace hd2d
