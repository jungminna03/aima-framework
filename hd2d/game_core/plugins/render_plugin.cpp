#include "game_core/plugins/render_plugin.h"

#include "assets/sprite_sheet.h"
#include "core/log_compat.h"
#include "renderer/frame_renderer.h"   // hd2d::FrameRenderer (고수준 씬 API — D3D12 대신)
#include "core/math_compat.h"
#include "game/components.h"
#include "game/physics_components.h"
#include "game/platform.h"             // sys::env (PAL env wrapper)
#include "game/res_path.h"             // res::sprite_path (해/달 발광 디스크 orb)
#include "game/resources.h"
#include "game/scene_enum.h"
#include "renderer/render_constants.h"   // FrameConstants (D3D12-free — forward_pass.h 끊음)

#if defined(HD2D_RENDERER_SDLGPU)
#include "renderer/device.h"            // Dx12Device::live_scene() (sdlgpu bridge)
#include "renderer/sdlgpu/live_scene.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace hd2d {

// Normalized time-of-day [0,1) = 게임 시계의 하루 분율(자정=0, 시각=t×24 — clock_hhmm과
// 동일 규약, 2026-07-10 통일). Defined at hd2d scope (external linkage) so other plugins —
// e.g. sound's day/night village music — can read it. GameTimeSystem(단일 권위)이 쓴다.
float g_tod = 0.0f;
// F2 시간 바가 특정 시각으로 점프시킬 때 여기 쓰면 다음 advance_time_of_day가 소비.
float g_tod_set = -1.0f;
void set_time_of_day(float t01) { g_tod_set = t01 - std::floor(t01); }
// GameTimeSystem(시간의 단일 권위)이 소비: 요청된 점프 시각을 반환(없으면 -1)하고 리셋.
float consume_time_of_day_jump() { const float j = g_tod_set; g_tod_set = -1.0f; return j; }

// F2 일차 점프 채널(2026-07-14, 사용자 "날짜 변경하는 기능 F2"): 하루 안 시각이 아니라
// **날짜(일차)를 통째로** ±N일 점프시키는 요청 — 재앙 일차(7/21/30) 인터랙티브 검증용.
// 시각 채널과 같은 관용구: 생산자(F2 버튼/테스트)는 request만, GameTimeSystem이
// accumulated_seconds += N×day_length로 소비(time_in_day 보존, 0 미만 클램프).
int g_day_jump = 0;
void request_day_jump(int days) { g_day_jump += days; }
int consume_day_jump() { const int j = g_day_jump; g_day_jump = 0; return j; }

// 시각(시계 시) 키프레임 워커: rows = {hour, r, g, b} 시각 오름차순. 세그먼트 안은
// smoothstep 보간, 마지막→첫 행은 자정을 감아 도는 랩 세그먼트(양끝이 같은 색이면 = 홀드).
// 구 등분(t*4) 커프레임과 달리 각 무드에 임의 시각·홀드 구간을 줄 수 있다.
static void tod_mood(float h, const float (*keys)[4], int n, float out[3]) {
    int i = n - 1;                                    // 기본: 랩 세그먼트(밤 홀드)
    for (int k = 0; k + 1 < n; ++k)
        if (h >= keys[k][0] && h < keys[k + 1][0]) { i = k; break; }
    const int j = (i + 1) % n;
    float h1 = keys[j][0], hh = h;
    if (j == 0) { h1 += 24.0f; if (hh < keys[i][0]) hh += 24.0f; }   // 자정 랩
    float u = (hh - keys[i][0]) / (h1 - keys[i][0]);
    u = std::clamp(u, 0.0f, 1.0f);
    u = u * u * (3.0f - 2.0f * u);                    // smoothstep
    for (int c = 0; c < 3; ++c)
        out[c] = keys[i][c + 1] * (1.0f - u) + keys[j][c + 1] * u;
}

// Day/night cycle (graphics-only, user 2026-06-20 / 시계 규약 통일 2026-07-10). t =
// 게임 시계의 하루 분율(자정=0, 시각 = t×24 — clock_hhmm·g_tod와 동일 규약)을 태양의
// "direction TO light"(엔진 공간) + 월드 앰비언트로 사상한다. 해는 06시 일출 → 12시
// 남중 → 18시 일몰(이후 지평선 아래 — 셰이더 max(ndl,0)가 어두운 앰비언트만 남긴다).
// 앰비언트 무드는 시각 키프레임: **밤은 정확히 20:00 도달, 05:00까지 홀드**(사용자
// 2026-07-10 "밤은 정확히 20시 이후" — 구 규약은 0=새벽06시 해석이라 표시 14:00에 밤이
// 렌더됐다). 태양광 색은 sdlgpu에서 고정 웜톤이라 앰비언트 틴트가 무드를 나른다.
// (hd2d 스코프 — HD2D_CLOCKTEST가 링크해 검증.)
void day_night_lighting(float t, float out_to_light[3], float out_ambient[3]) {
    const float kTau = 6.28318530718f;
    const float h = (t - std::floor(t)) * 24.0f;               // 시계 시각(자정=0)
    const float a = (h - 6.0f) / 24.0f * kTau;                 // 해: 06시 일출, 18시 일몰
    float dx = std::cos(a), dy = std::sin(a), dz = -0.35f;     // E–W arc + a Z tilt
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    out_to_light[0] = dx / len; out_to_light[1] = dy / len; out_to_light[2] = dz / len;

    static const float kAmb[][4] = {
        { 5.0f, 0.015f, 0.02f, 0.045f},  // 밤의 끝 — 05시까지 캄캄함 유지
        { 7.0f, 0.26f,  0.28f, 0.34f},   // 새벽 — cool twilight
        {12.0f, 0.55f,  0.55f, 0.52f},   // 정오 — bright daylight
        {17.0f, 0.55f,  0.55f, 0.52f},   // 오후 홀드 — 낮 밝기 유지(14시는 환한 낮)
        {19.0f, 0.42f,  0.22f, 0.10f},   // 황혼 — warm orange
        {20.0f, 0.015f, 0.02f, 0.045f},  // 밤 — 20:00 정각 도달(광원 외엔 캄캄)
    };
    tod_mood(h, kAmb, 6, out_ambient);
}

// Sky/background colour for the time-of-day = the clear colour of the HDR scene
// target. THE biggest day/night cue: without it the scene relights but the backdrop
// stays fixed, so it "doesn't feel like" day/night even though lighting changed
// (사용자 2026-06-23). 앰비언트와 같은 시각 키프레임(밤 = 20:00~05:00 홀드).
void day_night_sky(float t, float out[3]) {
    static const float kSky[][4] = {
        { 5.0f, 0.008f, 0.012f, 0.035f},  // 밤의 끝 — near-black navy
        { 7.0f, 0.42f,  0.46f,  0.58f},   // 새벽 — pale blue-violet
        {12.0f, 0.35f,  0.56f,  0.90f},   // 정오 — bright sky blue
        {17.0f, 0.35f,  0.56f,  0.90f},   // 오후 홀드
        {19.0f, 0.55f,  0.27f,  0.14f},   // 황혼 — burnt orange
        {20.0f, 0.008f, 0.012f, 0.035f},  // 밤 — 20:00 정각 도달
    };
    tod_mood((t - std::floor(t)) * 24.0f, kSky, 6, out);
}

namespace {

constexpr float kRad2Deg = 57.2957795f;

math::Matrix model_matrix(float scale, float yaw_deg, const Float3& pos) {
    return math::mul(
        math::mul(math::scaling(scale, scale, scale),
                             math::rotation_y(yaw_deg / kRad2Deg)),
        math::translation(pos.x, pos.y, pos.z));
}

constexpr float kPi = 3.14159265359f;

// Sun shadow state computed by ShadowPassSystem and consumed by
// BeginScenePassSystem in the same frame (plain namespace state is fine: both
// run on the render thread in registration order).
struct SunShadowState {
    bool active = false;
    int light_index = -1;
    math::Mat4x4 view_proj;
};
SunShadowState g_sun_shadow;

// (g_tod — the normalized time-of-day — is defined at hd2d scope above so other
// plugins can read it; the day/night lighting below reads it in this TU. Same
// "namespace state, render thread, registration order" deal as g_sun_shadow.)

// Direction the light travels, in engine space (= +row2 of the world matrix;
// the Z-flip conjugation flips the node-local Z axis sense).
Float3 light_dir_engine(const math::Mat4x4& m) {
    Float3 d{m.m[2][0], m.m[2][1], m.m[2][2]};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }
    return d;
}

// (advance_time_of_day 폐지 2026-07-01: 시간 전진은 GameTimeSystem이 단일 권위로 맡아
//  g_tod를 직접 구동한다 = 시간/날짜 시스템 ↔ 조명(비주얼) 통합. 아래 render 시스템들은
//  g_tod를 '읽기만' 한다. HD2D_DAYLEN/HD2D_TIMEOFDAY/F4 스크럽은 GameTimeSystem으로 이관,
//  F2 시간 바 점프는 set_time_of_day → consume_time_of_day_jump로 GameTime이 흡수한다.)

// ---------------------------------------------------------------------------
// 프러스텀 컬링(2026-07-15 사용자 "화면 밖은 렌더링 안 해서 드로우콜 줄이자"):
// 씬/그림자/페이드/빌보드 4개 드로우 루프가 맵 전 노드(medieval village_c = 2088개)를
// 매 프레임 무조건 그리던 것을, 뷰(또는 태양 ortho) 프러스텀 밖이면 스킵한다.
// 로컬 AABB는 엔티티별 1회 계산-캐시(맵 스왑/프림 교체 시 무효화), 매 프레임은
// 8코너 변환 + 6평면 테스트뿐. HD2D_NOCULL=1로 끄고(진단), HD2D_CULLSTATS=1이
// 300프레임마다 드로우/컬 카운트를 찍는다.
// ---------------------------------------------------------------------------
struct CullPlanes { float p[6][4]; };

// row-vector 규약(clip = p·VP, 이 코드베이스의 m[3][0..2]=이동과 일치): 평면 = VP 열 조합.
CullPlanes cull_planes_from(const math::Mat4x4& vp) {
    CullPlanes out{};
    auto set = [&](int i, float a, float b, float c, float d) {
        const float l = std::sqrt(a * a + b * b + c * c);
        const float inv = l > 1e-9f ? 1.0f / l : 0.0f;
        out.p[i][0] = a * inv; out.p[i][1] = b * inv;
        out.p[i][2] = c * inv; out.p[i][3] = d * inv;
    };
    const auto& m = vp.m;
    set(0, m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);   // left
    set(1, m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);   // right
    set(2, m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);   // bottom
    set(3, m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);   // top
    set(4, m[0][2], m[1][2], m[2][2], m[3][2]);                                            // near (z>=0)
    set(5, m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);   // far
    return out;
}

bool sphere_visible(const CullPlanes& f, const Float3& c, float r) {
    for (int i = 0; i < 6; ++i) {
        const float* pl = f.p[i];
        if (c.x * pl[0] + c.y * pl[1] + c.z * pl[2] + pl[3] < -r) return false;
    }
    return true;
}

bool aabb_visible(const CullPlanes& f, const Float3& mn, const Float3& mx) {
    const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f, cz = (mn.z + mx.z) * 0.5f;
    const float ex = (mx.x - mn.x) * 0.5f, ey = (mx.y - mn.y) * 0.5f, ez = (mx.z - mn.z) * 0.5f;
    for (int i = 0; i < 6; ++i) {
        const float* pl = f.p[i];
        const float r = ex * std::fabs(pl[0]) + ey * std::fabs(pl[1]) + ez * std::fabs(pl[2]);
        if (cx * pl[0] + cy * pl[1] + cz * pl[2] + pl[3] < -r) return false;
    }
    return true;
}

bool cull_enabled() {
    static const bool off = sys::env("HD2D_NOCULL") != nullptr;
    return !off;
}

// 엔티티별 로컬(모델공간) AABB 캐시. key=prims 벡터 데이터 포인터 — 엔티티 슬롯 재활용/
// 프롭 교체 시 자동 무효화, 맵 스왑 시 전체 클리어.
struct LocalBounds { const void* key; Float3 mn, mx; };
const LocalBounds& mesh_local_bounds(entt::entity e, const MeshRenderer& mr,
                                     const std::string& map_path) {
    static std::string s_map;
    static std::unordered_map<uint32_t, LocalBounds> s_cache;
    if (s_map != map_path) { s_cache.clear(); s_map = map_path; }
    LocalBounds& lb = s_cache[static_cast<uint32_t>(e)];
    if (lb.key != static_cast<const void*>(mr.prims.data())) {
        lb.key = mr.prims.data();
        Float3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (const LoadedPrimitive& prim : mr.prims)
            for (const Float3& p : prim.cpu.positions) {
                mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
                mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
            }
        if (mn.x > mx.x) { mn = Float3{0, 0, 0}; mx = Float3{0, 0, 0}; }
        lb.mn = mn; lb.mx = mx;
    }
    return lb;
}

// 로컬 AABB 8코너를 월드로 변환해 프러스텀 테스트(보수적 — 월드 AABB로 재합성).
bool mesh_visible(const CullPlanes& f, entt::entity e, const MeshRenderer& mr,
                  const math::Mat4x4& w, const std::string& map_path) {
    const LocalBounds& lb = mesh_local_bounds(e, mr, map_path);
    Float3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const float x = (i & 1) ? lb.mx.x : lb.mn.x;
        const float y = (i & 2) ? lb.mx.y : lb.mn.y;
        const float z = (i & 4) ? lb.mx.z : lb.mn.z;
        const float wx = x * w.m[0][0] + y * w.m[1][0] + z * w.m[2][0] + w.m[3][0];
        const float wy = x * w.m[0][1] + y * w.m[1][1] + z * w.m[2][1] + w.m[3][1];
        const float wz = x * w.m[0][2] + y * w.m[1][2] + z * w.m[2][2] + w.m[3][2];
        mn.x = std::min(mn.x, wx); mn.y = std::min(mn.y, wy); mn.z = std::min(mn.z, wz);
        mx.x = std::max(mx.x, wx); mx.y = std::max(mx.y, wy); mx.z = std::max(mx.z, wz);
    }
    return aabb_visible(f, mn, mx);
}

// HD2D_CULLSTATS: 300프레임마다 스킵 효율 로그(패스별 그린/컬 수).
struct CullStats { int drawn = 0, culled = 0; };
void cull_stats_log(const char* pass, CullStats& s) {
    static const bool on = sys::env("HD2D_CULLSTATS") != nullptr;
    if (!on) return;
    static std::unordered_map<std::string, int> s_frames;
    int& f = s_frames[pass];
    if (++f % 300 == 0)
        HD2D_INFO("[cull] {} drawn={} culled={}", pass, s.drawn, s.culled);
}

// Render the sun shadow map: pick the first directional map light, fit an
// orthographic frustum around the map bounds, draw every caster (meshes with
// their full material alpha state; billboards as alpha-tested cutouts rotated
// toward the light so they cast a real silhouette).
void ShadowPassSystem(Arimu::Query<MapLight, WorldMatrix> lights,
                      Arimu::Query<Transform, MeshRenderer> meshes,
                      Arimu::Query<WorldMatrix> world_mats,
                      Arimu::Query<NodeExtras> extras,
                      Arimu::Query<Transform, BillboardSprite> sprites,
                      Arimu::Res<MapScene> map,
                      Arimu::Res<RenderSettings> rs,
                      Arimu::Res<RenderContext> rc,
                      Arimu::Res<BillboardMesh> bbmesh,
                      Arimu::Res<Gfx> gfx,
                      Arimu::Query<Player, Transform> players) {
    g_sun_shadow.active = false;
    static const bool no_shadow = sys::env("HD2D_NOSHADOW") != nullptr;
    if (no_shadow) return;   // 진단: 그림자 OFF — 줄무늬가 사라지면 범인=그림자 확정
    if (!gfx->frame || !gfx->frame->shadow_ready()) return;

    // First directional light is THE sun.
    int sun_index = -1, idx = 0;
    Float3 sun_dir{};
    for (auto [e, ml, wm] : lights.each()) {
        if (idx >= static_cast<int>(FrameConstants::kMaxLights)) break;
        if (ml.light.type == 0 && sun_index < 0) {
            sun_index = idx;
            sun_dir = light_dir_engine(wm.m);
        }
        ++idx;
    }
    if (sun_index < 0) return;

    // Day/night: drive the shadow sun with the time-of-day direction so shadows
    // track the moving sun (matches BeginScenePassSystem's day/night lighting).
    {
        float to_light[3], amb[3];
        day_night_lighting(g_tod, to_light, amb);
        sun_dir = Float3{to_light[0], to_light[1], to_light[2]};
    }

    // Fit an ortho frustum: look down the sun at the bounds center, extents
    // from the bounds corners in light space.
    const Float3 mn = map->bounds_min, mx = map->bounds_max;
    // 그림자 밴딩의 두 근본 원인을 동시에: 프러스텀을 플레이어 주변으로 좁혀 (1) 텍셀을
    // 작게(해상도) + (2) ortho 깊이 범위를 타이트하게(부동소수점 정밀도). 맵 중심 폴백.
    Float3 focus{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
    for (auto [pe, ptf] : players.each()) { focus.x = ptf.position.x; focus.z = ptf.position.z; break; }
    const math::Vec center = math::vec(focus.x, focus.y, focus.z, 1.0f);
    float radius = 0.5f * std::sqrt((mx.x - mn.x) * (mx.x - mn.x) +
                                    (mx.y - mn.y) * (mx.y - mn.y) +
                                    (mx.z - mn.z) * (mx.z - mn.z)) + 1.0f;
    radius = std::min(radius, 30.0f);   // 플레이어 주변만 — 텍셀 작게 + 깊이범위 타이트
    const math::Vec dir = math::vec(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
    const math::Vec eye = math::sub(center, math::scale_v(dir, radius));
    const math::Vec up = std::fabs(sun_dir.y) > 0.95f ? math::vec(0, 0, 1, 0)
                                                         : math::vec(0, 1, 0, 0);
    const math::Matrix view = math::look_at_lh(eye, center, up);
    const math::Matrix proj = math::orthographic_lh(2.0f * radius, 2.0f * radius,
                                                         0.05f, 2.0f * radius + 1.0f);
    math::store(g_sun_shadow.view_proj, math::mul(view, proj));
    g_sun_shadow.light_index = sun_index;
    g_sun_shadow.active = true;

    gfx->frame->shadow_begin(g_sun_shadow.view_proj);

    // 태양 ortho 프러스텀 컬링: 그림자 프러스텀이 플레이어 반경 30m로 좁혀져 있어
    // (밴딩 수정) 맵 대부분의 캐스터를 통째로 스킵할 수 있다.
    const CullPlanes sun_planes = cull_planes_from(g_sun_shadow.view_proj);
    CullStats sstat;

    // Meshes (nocast extras opt out; MASK materials alpha-test their cutout).
    for (auto [e, tf, mr] : meshes.each()) {
        if (extras.Contains(e) && extras.Get<NodeExtras>(e).nocast) continue;
        math::Mat4x4 model;
        if (world_mats.Contains(e)) {
            model = world_mats.Get<WorldMatrix>(e).m;
        } else {
            math::store(model, model_matrix(tf.scale, tf.yaw_deg, tf.position));
        }
        if (cull_enabled() && !mesh_visible(sun_planes, e, mr, model, map->glb_path)) {
            ++sstat.culled; continue;
        }
        ++sstat.drawn;
        for (const LoadedPrimitive& prim : mr.prims) {
            float cutoff = 0.0f;
            rhi::GpuTexture base{};   // {0} → FrameRenderer가 fallback white로 해석
            if (prim.material >= 0 &&
                prim.material < static_cast<int>(map->materials.size())) {
                const LoadedMaterial& mat = map->materials[prim.material];
                if (mat.alpha_mode == 1) {
                    cutoff = mat.alpha_cutoff;
                    if (mat.tex_base >= 0 &&
                        mat.tex_base < static_cast<int>(map->textures.size()))
                        base = map->textures[mat.tex_base];
                }
            }
            gfx->frame->shadow_draw(prim.mesh, model, cutoff, base, nullptr, nullptr);
        }
    }

    // Billboards: rotate toward the light so the cutout casts a silhouette.
    const float yaw_to_light = std::atan2(-sun_dir.x, -sun_dir.z) * kRad2Deg;
    for (auto [e, tf, bb] : sprites.each()) {
        if (!bb.sheet.valid) continue;
        const float world_h = static_cast<float>(bb.sheet.frame_px) / rs->pixels_per_unit;
        if (cull_enabled() &&
            !sphere_visible(sun_planes, Float3{tf.position.x, tf.position.y + world_h * 0.5f,
                                               tf.position.z}, world_h)) {
            ++sstat.culled; continue;
        }
        ++sstat.drawn;
        math::Mat4x4 model;
        math::store(model, model_matrix(world_h, yaw_to_light, tf.position));
        float uv_off[2] = {bb.cur_frame / static_cast<float>(bb.sheet.frame_count),
                           bb.cur_dir / static_cast<float>(kSheetRows)};
        float uv_scale[2] = {1.0f / bb.sheet.frame_count, 1.0f / kSheetRows};
        gfx->frame->shadow_draw(bbmesh->quad, model, 0.5f, bb.sheet.texture, uv_off, uv_scale);
    }

    cull_stats_log("shadow", sstat);
    gfx->frame->shadow_end();
}

// Bind the HDR scene target + the forward pass, gather the map's punctual
// lights into the frame constants, and upload them. Everything after this
// draws into HDR; the PostFx system at the end of the phase resolves to the
// backbuffer.
void BeginScenePassSystem(Arimu::Query<MapLight, WorldMatrix> lights,
                          Arimu::Res<OrbitCamera> cam,
                          Arimu::Res<RenderSettings> rs,
                          Arimu::Res<MapScene> map,
                          Arimu::Res<RenderContext> rc,
                          Arimu::Res<FrameTime> time,
                          Arimu::Res<DebugState> dbg,
                          Arimu::Res<Gfx> gfx) {
    if (!gfx->frame || !gfx->frame->ready()) return;
    // 시간대 진행(프레임당 1회 단일 소스) + 하늘색(낮밤 최대 단서). fc/sky는 게임이
    // 만들고, 실제 GPU 셋업(타깃/클리어/뷰포트/forward begin)은 FrameRenderer가 한다.
    (void)time;   // 시간 전진은 GameTimeSystem(단일 권위)이 g_tod로 구동(자체 전진 폐지 2026-07-01)
    float sky[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    day_night_sky(g_tod, sky);

    const math::Matrix vp = math::mul(cam->view(), cam->proj(rc->aspect));
    FrameConstants fc{};
    math::store(fc.view_proj, vp);
    const Float3 cp = cam->position();
    fc.cam_pos[0] = cp.x; fc.cam_pos[1] = cp.y; fc.cam_pos[2] = cp.z;

    // Blender-authored lights (position = matrix row 3). A glTF light shines
    // along its node's glTF-local -Z; the Z-flip handedness conjugation also
    // flips the local Z axis, so in ENGINE space that direction is +row 2.
    uint32_t n = 0;
    for (auto [e, ml, wm] : lights.each()) {
        if (n >= FrameConstants::kMaxLights) break;
        const auto& m = wm.m;
        float dxx = m.m[2][0], dyy = m.m[2][1], dzz = m.m[2][2];
        const float len = std::sqrt(dxx * dxx + dyy * dyy + dzz * dzz);
        if (len > 1e-6f) { dxx /= len; dyy /= len; dzz /= len; }
        const LoadedLight& l = ml.light;
        float* pr = fc.light_pos_range[n];
        float* dt = fc.light_dir_type[n];
        float* ci = fc.light_color_inner[n];
        float* ou = fc.light_outer[n];
        pr[0] = m.m[3][0]; pr[1] = m.m[3][1]; pr[2] = m.m[3][2]; pr[3] = l.range;
        dt[0] = dxx; dt[1] = dyy; dt[2] = dzz; dt[3] = static_cast<float>(l.type);
        for (int c = 0; c < 3; ++c) ci[c] = l.color[c] * l.intensity_w;
        ci[3] = l.inner_cos;
        ou[0] = l.outer_cos;
        ++n;
    }
    // Maps without authored lights fall back to the legacy debug sun (x pi so
    // Lambert's /pi keeps the old perceived brightness).
    if (n == 0) {
        float dxx = rs->light_dir[0], dyy = rs->light_dir[1], dzz = rs->light_dir[2];
        const float len = std::sqrt(dxx * dxx + dyy * dyy + dzz * dzz);
        if (len > 1e-6f) { dxx /= len; dyy /= len; dzz /= len; }
        float* dt = fc.light_dir_type[0];
        float* ci = fc.light_color_inner[0];
        dt[0] = dxx; dt[1] = dyy; dt[2] = dzz; dt[3] = 0.0f;
        for (int c = 0; c < 3; ++c) ci[c] = rs->light_color[c] * kPi;
        n = 1;
    }
    fc.light_count = static_cast<float>(n);

    // World ambient: Blender world_color extras when present, panel otherwise.
    for (int c = 0; c < 3; ++c)
        fc.ambient[c] = map->has_world_ambient ? map->world_ambient[c] : rs->ambient[c];

    // Day/night cycle (graphics, PAL render — now on DX12 too): the time-of-day
    // drifts the sun direction + ambient mood (dawn-white → noon → dusk-orange →
    // night-blue). The shader already supports a dynamic directional sun + ambient.
    // F4 scrubs; HD2D_TIMEOFDAY pins. g_tod also feeds tod village music + shadows.
    {
        const float tod = g_tod;   // 위 클리어에서 이미 진행함(프레임당 1회 단일 소스)
        float to_light[3], amb[3];
        day_night_lighting(tod, to_light, amb);
        for (int c = 0; c < 3; ++c) fc.ambient[c] = amb[c];
        int sun = -1;
        for (uint32_t i = 0; i < n; ++i)
            if (fc.light_dir_type[i][3] == 0.0f) { sun = static_cast<int>(i); break; }
        if (sun < 0 && n < FrameConstants::kMaxLights) {  // 맵에 directional 없으면 낮밤 sun 추가
            sun = static_cast<int>(n);
            float* ci = fc.light_color_inner[n];
            for (int c = 0; c < 3; ++c) ci[c] = 3.0f;     // 따뜻한 흰 태양광
            ci[3] = 1.0f;
            ++n;
            fc.light_count = static_cast<float>(n);
        }
        if (sun >= 0) {
            fc.light_dir_type[sun][0] = -to_light[0];     // fc는 TRAVEL 방향 = -to_light
            fc.light_dir_type[sun][1] = -to_light[1];
            fc.light_dir_type[sun][2] = -to_light[2];
            fc.light_dir_type[sun][3] = 0.0f;
            // 태양 세기를 강한 따뜻한 햇빛으로 통일 → 낮이 확실히 밝게(저자 sun ci≈2.2는
            // (albedo/π)*ci 라 정오에도 ~제 색). 밤엔 해가 지평선 아래(ndl≤0)라 night는
            // 안 밝힘 — 낮 밝기/밤 어둠 대비는 ci(낮)와 night 앰비언트(0.015~)로 결정.
            float* ci = fc.light_color_inner[sun];
            ci[0] = 5.0f; ci[1] = 4.65f; ci[2] = 4.0f;    // warm daylight (~5)
        }
        static int s_dnlog = 0;                            // 진단: tod가 도는지 hd2d.log로 확인
        if ((s_dnlog++ % 60) == 0)
            HD2D_INFO("[daynight] tod={:.2f} scrub={} sun_slot={} amb=({:.2f},{:.2f},{:.2f})",
                      tod, dbg->time_scrub ? 1 : 0, sun, amb[0], amb[1], amb[2]);
    }

    // Sun shadow (rendered by ShadowPassSystem just before this system).
    fc.shadow_params[3] = -1.0f;
    if (g_sun_shadow.active) {
        fc.sun_view_proj = g_sun_shadow.view_proj;
        fc.shadow_params[0] = 1.0f / static_cast<float>(ShadowPass::kSize);
        fc.shadow_params[1] = 0.005f;  // depth bias (작은 텍셀이라 낮춰 — peter-panning↓)
        fc.shadow_params[2] = 0.12f;   // normal-offset world units
        fc.shadow_params[3] = static_cast<float>(g_sun_shadow.light_index);
    }

    gfx->frame->scene_begin(fc, sky, rc->width, rc->height);
}

// Resolve HDR -> backbuffer (bloom + tonemap). Last render system: ImGui draws
// after this onto the backbuffer the pass leaves bound. PAL keystone: now goes
// through the high-level FrameRenderer (no raw D3D12 here).
void PostFxSystem(Arimu::Res<RenderSettings> rs,
                  Arimu::Res<Gfx> gfx) {
    if (gfx->frame) gfx->frame->post(rs->post);
}

// (map_srv 제거 — 텍스처 SRV 해석은 FrameRenderer::scene_draw_mesh 내부로 이동.)

// Compose a mesh entity's world matrix: simulated rigid bodies = authored-scale *
// quat * pos; map nodes = the Blender WorldMatrix; anything else = simple Transform.
template <class WMQuery, class RotQuery>
math::Matrix mesh_world_matrix(entt::entity e, const Transform& tf,
                               WMQuery& world_mats, RotQuery& rotations) {
    if (rotations.Contains(e)) {
        const Rotation3D& r = rotations.template Get<Rotation3D>(e);
        math::Vec s = math::vec(1, 1, 1, 0);
        if (world_mats.Contains(e)) {
            const auto& w = world_mats.template Get<WorldMatrix>(e).m;
            auto len = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
            s = math::vec(len(w.m[0][0], w.m[0][1], w.m[0][2]),
                                len(w.m[1][0], w.m[1][1], w.m[1][2]),
                                len(w.m[2][0], w.m[2][1], w.m[2][2]), 0);
        }
        return math::scaling_v(s) *
               math::rotation_quat(math::load4(r.quat)) *
               math::translation(tf.position.x, tf.position.y, tf.position.z);
    }
    if (world_mats.Contains(e)) return math::load(world_mats.template Get<WorldMatrix>(e).m);
    return model_matrix(tf.scale, tf.yaw_deg, tf.position);
}

// Issue the per-primitive draws for one mesh's MeshRenderer at world matrix m.
// alpha < 1 = camera-occlusion fade (the CALLER brackets with scene_set_translucent).
// Backend-neutral: builds a DrawMaterial (rhi handles + PBR factors) and hands each
// primitive to the FrameRenderer, which resolves handles + records the GPU draw.
// two_sided: 호출측이 NodeExtras.cloth로 결정(2026-07-15) — 재질 doubleSided 플래그는
// 생성기들이 전 재질에 무차별로 켜서(medieval 25/26, overworld 16/16) 뒷면 컬링을
// 통째로 무효화했다. 실제로 양면이 필요한 건 천(배너) 같은 얇은 시뮬 메시뿐.
void draw_mesh_prims(const Gfx& gfx, const MapScene& map,
                     const math::Matrix& m, const MeshRenderer& mr, float alpha,
                     bool two_sided = false) {
    for (const LoadedPrimitive& prim : mr.prims) {
        DrawMaterial dm{};
        const LoadedMaterial* mat =
            (prim.material >= 0 && prim.material < static_cast<int>(map.materials.size()))
                ? &map.materials[prim.material]
                : nullptr;
        if (mat) {
            dm.has_material = true;
            for (int c = 0; c < 4; ++c) dm.base_color[c] = mat->base_color[c];
            dm.metallic = mat->metallic;
            dm.roughness = mat->roughness;
            dm.alpha_cutoff = mat->alpha_mode == 1 ? mat->alpha_cutoff : 0.0f;
            dm.flags = static_cast<uint32_t>(mat->sampler_flags);
            if (mat->tex_normal >= 0) dm.flags |= 4;
            for (int c = 0; c < 3; ++c)
                dm.emissive_factor[c] = mat->emissive[c] * mat->emissive_strength;
            auto tex = [&](int idx) -> rhi::GpuTexture {
                return (idx >= 0 && idx < static_cast<int>(map.textures.size()))
                           ? map.textures[idx] : rhi::GpuTexture{};
            };
            dm.base = tex(mat->tex_base);
            dm.mr = tex(mat->tex_mr);
            dm.normal = tex(mat->tex_normal);
            dm.emissive = tex(mat->tex_emissive);
        }
        if (two_sided) dm.flags |= 16;   // cull NONE PSO 선택(renderer_impl 비트16)
        gfx.frame->scene_draw_mesh(prim.mesh, m, dm, alpha);
    }
}

// 3D 스카이박스(스펙 2026-07-15): BeginScenePass 직후, 모든 드로우보다 먼저.
// depth OFF PSO + unlit(flags bit8) — 이후 씬이 하늘을 덮는다. 카메라를 63/64만큼
// 따라가(이동의 1/64만 시차) 회전은 뷰 행렬이 처리. 색 = 에셋(정오 기준) ×
// day_night_sky/정오 정규화 틴트(밤엔 어둡게 — 낮밤 단서 유지).
void SkyRenderSystem(Arimu::Res<SkyScene> sky,
                     Arimu::Res<OrbitCamera> cam,
                     Arimu::Res<Gfx> gfx) {
    if (!sky->valid || !gfx->frame || !gfx->frame->ready()) return;

    float dn[3]; day_night_sky(g_tod, dn);
    static const float kNoon[3] = {0.35f, 0.56f, 0.90f};   // day_night_sky 정오 키
    float tint[3];
    for (int c = 0; c < 3; ++c) tint[c] = dn[c] / kNoon[c];

    const Float3 cp = cam->position();
    const float k = 1.0f - 1.0f / 64.0f;   // 패럴랙스: 이동의 1/64만 보임
    const math::Matrix follow = math::translation(cp.x * k, cp.y * k, cp.z * k);

    gfx->frame->scene_set_sky(true);
    uint32_t drawn = 0;
    for (const SkyNode& n : sky->nodes) {
        const math::Matrix m = math::load(n.world) * follow;
        for (const LoadedPrimitive& prim : n.prims) {
            DrawMaterial dm{};
            dm.has_material = true;
            dm.flags = 8;   // unlit
            const LoadedMaterial* mat =
                (prim.material >= 0 && prim.material < (int)sky->materials.size())
                    ? &sky->materials[prim.material] : nullptr;
            for (int c = 0; c < 4; ++c) dm.base_color[c] = mat ? mat->base_color[c] : 1.0f;
            for (int c = 0; c < 3; ++c) dm.base_color[c] *= tint[c];
            if (mat) {
                dm.flags |= (uint32_t)mat->sampler_flags & 3u;   // nearest/clamp만 승계
                for (int c = 0; c < 3; ++c)
                    dm.emissive_factor[c] = mat->emissive[c] * mat->emissive_strength;
                if (mat->tex_base >= 0 && mat->tex_base < (int)sky->textures.size())
                    dm.base = sky->textures[mat->tex_base];
                if (mat->tex_emissive >= 0 && mat->tex_emissive < (int)sky->textures.size())
                    dm.emissive = sky->textures[mat->tex_emissive];
            }
            gfx->frame->scene_draw_mesh(prim.mesh, m, dm, 1.0f);
            ++drawn;
        }
    }
    gfx->frame->scene_set_sky(false);
    static bool logged = false;
    if (!logged) { logged = true; HD2D_INFO("[sky] draw n={}", drawn); }
}

// Draw the low-poly glTF meshes with their full PBR material set. Meshes currently
// occluding the player (OcclusionState, alpha<1) are SKIPPED here — drawn translucent
// by OccluderFadeRenderSystem after the player billboard so the character shows.
void MeshRenderSystem(Arimu::Query<Transform, MeshRenderer> meshes,
                      Arimu::Query<WorldMatrix> world_mats,
                      Arimu::Query<Rotation3D> rotations,
                      Arimu::Query<NodeExtras> extras,
                      Arimu::Res<MapScene> map,
                      Arimu::Res<OrbitCamera> cam,
                      Arimu::Res<RenderContext> rc,
                      Arimu::Res<Gfx> gfx,
                      Arimu::Res<OcclusionState> occ) {
    if (!gfx->frame || !gfx->frame->ready()) return;
    // 카메라 프러스텀 컬링(2026-07-15): 화면 밖 노드는 드로우콜 자체를 스킵.
    math::Mat4x4 vpm;
    math::store(vpm, math::mul(cam->view(), cam->proj(rc->aspect)));
    const CullPlanes planes = cull_planes_from(vpm);
    CullStats stat;
    for (auto [e, tf, mr] : meshes.each()) {
        if (occ->at(static_cast<uint32_t>(e)) < 0.999f) continue;   // faded -> other pass
        const math::Matrix m = mesh_world_matrix(e, tf, world_mats, rotations);
        if (cull_enabled()) {
            math::Mat4x4 w;
            math::store(w, m);
            if (!mesh_visible(planes, e, mr, w, map->glb_path)) { ++stat.culled; continue; }
        }
        ++stat.drawn;
        const bool cloth = extras.Contains(e) && extras.Get<NodeExtras>(e).cloth;
        draw_mesh_prims(*gfx, *map, m, mr, 1.0f, cloth);
    }
    cull_stats_log("scene", stat);
}

// Draw the billboard pixel sprites through the same pass. The quad faces the
// camera; the 8-direction row + animation frame were chosen in Logic-phase
// systems, so this only reads them.
void BillboardRenderSystem(Arimu::Query<Transform, BillboardSprite> sprites,
                           Arimu::Res<OrbitCamera> cam,
                           Arimu::Res<RenderSettings> rs,
                           Arimu::Res<RenderContext> rc,
                           Arimu::Res<Gfx> gfx,
                           Arimu::Res<MapScene> map,
                           Arimu::Res<BillboardMesh> mesh) {
    if (!gfx->frame || !gfx->frame->ready()) return;
    const Float3 cp = cam->position();
    math::Mat4x4 vpm;
    math::store(vpm, math::mul(cam->view(), cam->proj(rc->aspect)));
    const CullPlanes planes = cull_planes_from(vpm);
    for (auto [e, tf, bb] : sprites.each()) {
        if (!bb.sheet.valid) continue;
        {   // 화면 밖 빌보드 컬링(높이의 구 근사 — 페이퍼 오프셋/스쿼시 여유 포함)
            const float rr = static_cast<float>(bb.sheet.frame_px) / rs->pixels_per_unit *
                             std::max(1.0f, tf.scale) * 1.2f;
            if (cull_enabled() &&
                !sphere_visible(planes, Float3{tf.position.x, tf.position.y + rr * 0.5f,
                                               tf.position.z}, rr))
                continue;
        }

        const float dx_ = cp.x - tf.position.x;
        const float dz_ = cp.z - tf.position.z;
        const float yaw_to_cam = std::atan2(dx_, dz_) * kRad2Deg;  // quad faces camera

        // Transform.scale 반영(2026-07-07): SDL_GPU 경로(:745)에만 배선돼 있고 DX12
        // 실경로는 무시하고 있었다 — 파이어볼 스케일/드롭 0.85/시설 데코 스케일이
        // 이 머신(D3D12)에서 전부 무효였던 근본 원인.
        const float world_h = static_cast<float>(bb.sheet.frame_px) / rs->pixels_per_unit *
                              (tf.scale > 0.0f ? tf.scale : 1.0f);
        const Float3 ppos = {tf.position.x + bb.off_x, tf.position.y + bb.bob_y,
                             tf.position.z + bb.off_z};
        // 페이퍼 스쿼시&스트레치(2026-07-16): SDL_GPU 경로(geometry_pass :874)와 동일하게
        // 비균일 스케일(발밑 y=0 고정) — 이게 빠져 있어 Windows(DX12)에서만 사망
        // 납작/가라앉기 연출이 안 보였다.
        const math::Matrix m = math::mul(
            math::mul(math::scaling(world_h * bb.scale_x, world_h * bb.scale_y, world_h),
                      math::rotation_y(yaw_to_cam / kRad2Deg)),
            math::translation(ppos.x, ppos.y, ppos.z));

        // Combat tint: written each frame by the game-side CombatTintSystem
        // (피격 플래시 / 사망 암전 / 파이어볼 HDR 발광 — 승격 역전으로 전투 상태를
        // 렌더가 직접 읽지 않는다). 여기선 곱해 그릴 뿐.
        const float r = bb.tint_r, g = bb.tint_g, b = bb.tint_b;

        float us[2] = {1.0f / static_cast<float>(bb.sheet.frame_count),
                       1.0f / static_cast<float>(kSheetRows)};
        float uo[2] = {static_cast<float>(bb.cur_frame) * us[0],
                       static_cast<float>(bb.cur_dir) * us[1]};
        if (bb.flip_x) { uo[0] += us[0]; us[0] = -us[0]; }   // 페이퍼 좌우 플립
        const float tint[4] = {r, g, b, 1.0f};
        gfx->frame->scene_draw_sprite(mesh->quad, m, bb.sheet.texture, uo, us,
                                      tint, 0.5f, 1u | 2u);   // pixel-art: alpha-test, point+clamp
    }

    // (Night fireflies 삭제 2026-07-10 — 사용자 "반딧불이 없애봐". 밤 발광 모트 방출을
    //  DX12/sdlgpu 양쪽에서 제거. firefly 스프라이트 에셋은 아래 해/달 디스크가 계속 쓴다.)

    // --- 해 + 달 (시간 파악): day_night 태양 방향에 큰 발광 디스크. 해=낮 / 달=밤(반대편). ---
    {
        static SpriteSheet s_orb; static bool s_orb_tried = false;
        if (!s_orb_tried) {
            s_orb_tried = true;
            load_sprite_sheet(*gfx->device, *gfx->table, res::sprite_path("firefly"), s_orb);
        }
        float tl[3], amb2[3];
        day_night_lighting(g_tod, tl, amb2);
        const float R = 90.0f;                      // 하늘 거리(far plane 안)
        const float uo[2] = {0.0f, 0.0f}, us[2] = {1.0f, 1.0f};
        // 해: to_light 방향, 지평선 위일 때 보임(높을수록 밝게).
        {
            const float a = std::clamp((tl[1] + 0.05f) / 0.25f, 0.0f, 1.0f);
            if (a > 0.01f) {
                const Float3 p{cp.x + tl[0] * R, cp.y + tl[1] * R, cp.z + tl[2] * R};
                const float yaw = std::atan2(cp.x - p.x, cp.z - p.z) * kRad2Deg;
                const math::Matrix m = model_matrix(16.0f, yaw, p);
                const float e = 9.0f * a;
                const float tint[4] = {1.00f * e, 0.93f * e, 0.65f * e, 1.0f};   // 따뜻한 해
                gfx->frame->scene_draw_sprite(mesh->quad, m, s_orb.texture, uo, us, tint, 0.04f, 2u);
            }
        }
        // 달: 반대편(anti-sun) — 해가 지면(밤) 떠오름.
        {
            const float a = std::clamp((-tl[1] + 0.05f) / 0.25f, 0.0f, 1.0f);
            if (a > 0.01f) {
                const Float3 p{cp.x - tl[0] * R, cp.y - tl[1] * R, cp.z - tl[2] * R};
                const float yaw = std::atan2(cp.x - p.x, cp.z - p.z) * kRad2Deg;
                const math::Matrix m = model_matrix(11.0f, yaw, p);
                const float e = 2.6f * a;
                const float tint[4] = {0.78f * e, 0.85f * e, 1.00f * e, 1.0f};   // 차가운 달
                gfx->frame->scene_draw_sprite(mesh->quad, m, s_orb.texture, uo, us, tint, 0.04f, 2u);
            }
        }
    }
}

#if !defined(HD2D_RENDERER_SDLGPU)
// Camera-occlusion fade pass (DX12): redraw the meshes MeshRenderSystem skipped
// (OcclusionState alpha<1) with the blend PSO, AFTER the player billboard, so the
// wall blends translucently OVER the character ("관통"). Depth-test (no write) keeps
// it correctly behind nearer geometry; depth-write-off avoids occluding later draws.
void OccluderFadeRenderSystem(Arimu::Query<Transform, MeshRenderer> meshes,
                              Arimu::Query<WorldMatrix> world_mats,
                              Arimu::Query<Rotation3D> rotations,
                              Arimu::Res<MapScene> map,
                              Arimu::Res<OrbitCamera> cam,
                              Arimu::Res<RenderContext> rc,
                              Arimu::Res<Gfx> gfx,
                              Arimu::Res<OcclusionState> occ) {
    if (!gfx->frame || !gfx->frame->ready() || occ->alpha.empty()) return;
    math::Mat4x4 vpm;
    math::store(vpm, math::mul(cam->view(), cam->proj(rc->aspect)));
    const CullPlanes planes = cull_planes_from(vpm);
    gfx->frame->scene_set_translucent(true);
    for (auto [e, tf, mr] : meshes.each()) {
        const float a = occ->at(static_cast<uint32_t>(e));
        if (a >= 0.999f) continue;
        const math::Matrix m = mesh_world_matrix(e, tf, world_mats, rotations);
        if (cull_enabled()) {
            math::Mat4x4 w;
            math::store(w, m);
            if (!mesh_visible(planes, e, mr, w, map->glb_path)) continue;
        }
        draw_mesh_prims(*gfx, *map, m, mr, a);
    }
    gfx->frame->scene_set_translucent(false);   // restore the opaque PSO
}
#endif

#if defined(HD2D_RENDERER_SDLGPU)
// ---------------------------------------------------------------------------
// SDL_GPU live-scene render system (macOS/Linux backend, rungs 1 & 2).
//
// The DX12-coupled systems above all early-return here (rc->gpu_cmd is null on
// this backend). This ONE system instead reads the SAME ECS components and
// fills the device's LiveScene with plain data — the live OrbitCamera, the real
// MapScene meshes at their world transforms, the punctual light, and every
// BillboardSprite as a camera-facing textured quad. The device renders it inside
// begin_frame (see sdlgpu/device.cpp + geometry_pass.cpp). So what's on screen
// is the ACTUAL game world from the player's camera, not a fixed glb framing.
// ---------------------------------------------------------------------------
void SdlGpuLiveSceneSystem(Arimu::Query<Transform, MeshRenderer> meshes,
                           Arimu::Query<WorldMatrix> world_mats,
                           Arimu::Query<Transform, BillboardSprite> sprites,
                           Arimu::Query<MapLight, WorldMatrix> lights,
                           Arimu::Res<OrbitCamera> cam,
                           Arimu::Res<RenderSettings> rs,
                           Arimu::Res<MapScene> map,
                           Arimu::Res<RenderContext> rc,
                           Arimu::Res<OcclusionState> occ,
                           Arimu::Res<Gfx> gfx) {
    if (!gfx->device) return;
    LiveScene& live = gfx->device->live_scene();
    live.clear();

    // Camera (live OrbitCamera — follows the player).
    math::store(live.view, cam->view());
    math::store(live.proj, cam->proj(rc->aspect));
    const Float3 cp = cam->position();
    live.cam_pos = cp;

    // Lighting: first directional MapLight (direction TO the light = -travel dir,
    // engine space), else the RenderSettings debug sun.
    bool have_light = false;
    for (auto [e, ml, wm] : lights.each()) {
        if (ml.light.type != 0) continue;       // directional only for the key light
        const auto& m = wm.m;
        float dxx = m.m[2][0], dyy = m.m[2][1], dzz = m.m[2][2];   // light TRAVEL dir
        const float len = std::sqrt(dxx * dxx + dyy * dyy + dzz * dzz);
        if (len > 1e-6f) { dxx /= len; dyy /= len; dzz /= len; }
        live.light_dir[0] = -dxx; live.light_dir[1] = -dyy; live.light_dir[2] = -dzz;
        have_light = true;
        break;
    }
    if (!have_light) {
        // RenderSettings.light_dir is the TRAVEL direction; negate for "to light".
        float dxx = rs->light_dir[0], dyy = rs->light_dir[1], dzz = rs->light_dir[2];
        const float len = std::sqrt(dxx * dxx + dyy * dyy + dzz * dzz);
        if (len > 1e-6f) { dxx /= len; dyy /= len; dzz /= len; }
        live.light_dir[0] = -dxx; live.light_dir[1] = -dyy; live.light_dir[2] = -dzz;
    }
    for (int c = 0; c < 3; ++c)
        live.ambient[c] = map->has_world_ambient ? map->world_ambient[c] : rs->ambient[c];

    // --- Day/night cycle (graphics-only, user 2026-06-20) --------------------
    // Override the authored map sun/ambient with the time-of-day cycle: g_tod =
    // 게임 시계 하루 분율(자정=0) — GameTimeSystem(단일 권위)이 구동하고, HD2D_TIMEOFDAY가
    // 스크린샷용으로 핀한다(같은 규약: 0=자정 .5=정오 .8333=20시=밤 도달). 무드 커브는
    // day_night_lighting(시각 키프레임, 밤 = 20:00 정각~05:00 홀드).
    {
        const float tod = g_tod;
        float to_light[3], amb[3];
        day_night_lighting(tod, to_light, amb);
        for (int c = 0; c < 3; ++c) {
            live.light_dir[c] = to_light[c];
            live.ambient[c]   = amb[c];
        }
    }

    // Sun shadow framing: fit an ortho frustum looking down the sun at the map
    // bounds center (identical math to the DX12 ShadowPassSystem). live.light_dir
    // is the direction TO the light; the sun TRAVELS the opposite way.
    live.bounds_min = map->bounds_min;
    live.bounds_max = map->bounds_max;
    {
        const Float3 mn = map->bounds_min, mx = map->bounds_max;
        const math::Vec center = math::vec((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f,
                                                    (mn.z + mx.z) * 0.5f, 1.0f);
        const float radius = 0.5f * std::sqrt((mx.x - mn.x) * (mx.x - mn.x) +
                                              (mx.y - mn.y) * (mx.y - mn.y) +
                                              (mx.z - mn.z) * (mx.z - mn.z)) + 1.0f;
        // Sun travel direction = -(to light).
        const math::Vec dir = math::vec(-live.light_dir[0], -live.light_dir[1],
                                                 -live.light_dir[2], 0.0f);
        const math::Vec eye = math::sub(center, math::scale_v(dir, radius));
        const bool steep = std::fabs(live.light_dir[1]) > 0.95f;
        const math::Vec up = steep ? math::vec(0, 0, 1, 0)
                                      : math::vec(0, 1, 0, 0);
        const math::Matrix view = math::look_at_lh(eye, center, up);
        const math::Matrix proj = math::orthographic_lh(2.0f * radius, 2.0f * radius,
                                                             0.05f, 2.0f * radius + 1.0f);
        math::store(live.sun_view_proj, math::mul(view, proj));
        live.shadow_active = (radius > 1e-3f);
    }

    // World meshes: each MeshRenderer prim with its world matrix + material color.
    // Camera-occlusion fade: OcclusionState (physics OcclusionDetect raycast)
    // carries per-entity alpha; faded meshes render translucent after billboards.
    for (auto [e, tf, mr] : meshes.each()) {
        const float occ_alpha = occ->at(static_cast<uint32_t>(e));
        math::Mat4x4 model;
        if (world_mats.Contains(e)) {
            model = world_mats.Get<WorldMatrix>(e).m;
        } else {
            math::store(model, model_matrix(tf.scale, tf.yaw_deg, tf.position));
        }
        for (const LoadedPrimitive& prim : mr.prims) {
            if (prim.cpu.positions.empty() || prim.cpu.indices.size() < 3) continue;
            LiveMesh lm;
            lm.positions = &prim.cpu.positions;
            lm.indices = &prim.cpu.indices;
            lm.model = model;
            if (prim.material >= 0 &&
                prim.material < static_cast<int>(map->materials.size())) {
                const LoadedMaterial& mat = map->materials[prim.material];
                lm.color[0] = mat.base_color[0];
                lm.color[1] = mat.base_color[1];
                lm.color[2] = mat.base_color[2];
            }
            lm.alpha = occ_alpha;
            live.meshes.push_back(std::move(lm));
        }
    }

    // Billboards: camera-facing textured quads at the entity's position. The
    // animation frame / facing row were chosen by the sprite systems; the device
    // loads the sheet by path (BillboardSprite.path) and caches the texture.
    for (auto [e, tf, bb] : sprites.each()) {
        if (bb.path.empty()) continue;
        // The sheet may not have loaded its dimensions (frame_px) on this backend
        // — the SpriteLoadSystem upload stub fails — so derive frame geometry from
        // the sheet if present, else assume a standard 8-row sheet sized later by
        // the device. We still need frame_px for the world height; the sprite
        // systems fill bb.sheet.frame_px even when valid is false? No — the load
        // sets it before the GPU upload. Fall back to 1 frame if unknown.
        const int frame_count = bb.sheet.frame_count > 0 ? bb.sheet.frame_count : 1;
        const int frame_px = bb.sheet.frame_px > 0 ? bb.sheet.frame_px
                                                    : static_cast<int>(rs->pixels_per_unit);
        const float dx_ = cp.x - tf.position.x;
        const float dz_ = cp.z - tf.position.z;
        const float yaw_to_cam = std::atan2(dx_, dz_) * kRad2Deg;

        LiveBillboard lb;
        lb.sheet_path = bb.path;
        lb.position = tf.position;
        lb.position.x += bb.off_x;    // 페이퍼: 런지/흔들림(수평)
        lb.position.y += bb.bob_y;    // 페이퍼: 홉/가라앉기(수직)
        lb.position.z += bb.off_z;
        lb.yaw_deg = yaw_to_cam;
        lb.world_height = static_cast<float>(frame_px) / rs->pixels_per_unit * tf.scale;   // 시설/데코 빌보드 크기(Transform.scale) — SDL_GPU LiveScene 경로 전용(직접 DX12 폴백 :226/:461은 사장 코드, 미적용)
        lb.scale_x = bb.scale_x;      // 페이퍼: 스쿼시&스트레치(비균일 스케일)
        lb.scale_y = bb.scale_y;
        lb.uv_scale[0] = 1.0f / static_cast<float>(frame_count);
        lb.uv_scale[1] = 1.0f / static_cast<float>(kSheetRows);
        lb.uv_offset[0] = static_cast<float>(bb.cur_frame) * lb.uv_scale[0];
        lb.uv_offset[1] = static_cast<float>(bb.cur_dir) * lb.uv_scale[1];
        if (bb.flip_x) {              // 왼쪽 방향 = 셀 내 U 미러(구조체 변경 없이)
            lb.uv_offset[0] += lb.uv_scale[0];
            lb.uv_scale[0]  = -lb.uv_scale[0];
        }

        // Combat tint written by the game-side CombatTintSystem (승격 역전).
        // ⚠️ 구 SDL 경로는 파이어볼 발광을 안 그렸으나, 이제 tint 단일 소스라 DX12와
        // 동일하게 파이어볼 HDR 발광도 반영된다(일관성 정정 — 곱셈 소비만).
        lb.tint[0] = bb.tint_r; lb.tint[1] = bb.tint_g; lb.tint[2] = bb.tint_b;
        live.billboards.push_back(std::move(lb));
    }

    // (Night fireflies 삭제 2026-07-10 — 사용자 "반딧불이 없애봐". 밤 발광 모트 방출을
    //  DX12/sdlgpu 양쪽에서 제거.)

    live.valid = !live.meshes.empty() || !live.billboards.empty();
}
#endif  // HD2D_RENDERER_SDLGPU

} // namespace

void RenderPlugin::Build(Arimu::App& app) {
    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(ShadowPassSystem,       scene, Arimu::Phase::Render, "ShadowPass");
    app.AddSystem(BeginScenePassSystem,   scene, Arimu::Phase::Render, "BeginScenePass");
    app.AddSystem(SkyRenderSystem,        scene, Arimu::Phase::Render, "SkyRender");   // 하늘 먼저(depth off)
    app.AddSystem(MeshRenderSystem,       scene, Arimu::Phase::Render, "MeshRender");
    app.AddSystem(BillboardRenderSystem,  scene, Arimu::Phase::Render, "BillboardRender");
#if !defined(HD2D_RENDERER_SDLGPU)
    app.AddSystem(OccluderFadeRenderSystem, scene, Arimu::Phase::Render, "OccluderFade");
#endif
    app.AddSystem(PostFxSystem,           scene, Arimu::Phase::Render, "PostFx");
#if defined(HD2D_RENDERER_SDLGPU)
    // The live SDL_GPU bridge: fills the device's LiveScene from the ECS so the
    // SDL_GPU backend renders the ACTUAL game (the DX12 systems above no-op here).
    app.AddSystem(SdlGpuLiveSceneSystem,  scene, Arimu::Phase::Render, "SdlGpuLiveScene");
#endif
}

} // namespace hd2d
