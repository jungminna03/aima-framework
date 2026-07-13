#include "game_core/plugins/sprite_plugin.h"

#include "assets/sprite_sheet.h"
#include "game/combat_components.h"
#include "game/components.h"
#include "game/paper_anim.h"
#include "game/resources.h"
#include "game/scene_enum.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace hd2d {

namespace {

constexpr float kRad2Deg = 57.2957795f;
constexpr float kDeg2Rad = 0.0174532925f;

// Deferred load, CACHED BY PATH (user-machine freeze, 2026-06-22): each unique
// sheet uploads to the GPU exactly once and every entity reuses that handle. The
// upload (upload_texture -> immediate_submit) does a FULL GPU stall, so loading
// per-entity meant every projectile spawn / every unit at combat-start fired a
// mid-frame GPU flush — a burst of stalls that froze the machine. GpuTexture is a
// refcounted ComPtr handle, so copying the cached sheet into each BillboardSprite
// is cheap and shares one texture (the cache keeps it alive). Static = single-
// threaded system exec (PreLogic), survives DLL hot-reload via the leaked module.
void SpriteLoadSystem(Arimu::Query<BillboardSprite> sprites, Arimu::Res<Gfx> gfx) {
    static std::unordered_map<std::string, SpriteSheet> s_cache;
    for (auto [e, bb] : sprites.each()) {
        if (bb.load_attempted) continue;
        bb.load_attempted = true;
        auto it = s_cache.find(bb.path);
        if (it != s_cache.end()) { bb.sheet = it->second; continue; }   // reuse — no upload
        SpriteSheet sheet;
        load_sprite_sheet(*gfx->device, *gfx->table, bb.path, sheet);
        if (sheet.valid) s_cache.emplace(bb.path, sheet);   // cache only successful uploads
        bb.sheet = sheet;
    }
}

// Orthogonal locomotion axis: derive Idle/Walk + this frame's speed from the
// displacement, so it's computed identically for the player and every AI. Runs
// after movement. speed feeds the paper-animation hop phase.
void LocomotionSystem(Arimu::Query<Transform, Locomotion> movers, Arimu::Res<FrameTime> time) {
    const float dt = time->dt;
    for (auto [e, tf, loco] : movers.each()) {
        if (!loco.seeded) {
            loco.prev_pos = tf.position;
            loco.seeded = true;
            loco.move = Locomotion::Move::Idle;
            loco.speed = 0.0f;
            continue;
        }
        const float dx = tf.position.x - loco.prev_pos.x;
        const float dz = tf.position.z - loco.prev_pos.z;
        const float speed = (dt > 1e-4f) ? std::sqrt(dx * dx + dz * dz) / dt : 0.0f;
        loco.speed = speed;
        loco.move = (speed > 0.4f) ? Locomotion::Move::Walk : Locomotion::Move::Idle;
        loco.prev_pos = tf.position;
    }
}

// Committed actions that translate the body forward (dodge / attack lunge).
bool lunges(Action a) {
    return a == Action::Attack || a == Action::Bomb || a == Action::Roll;
}

// Normalized [0,1] phase of a committed action from the ONE combat clock
// (CombatState.timer) vs the action's (Speed-scaled) total duration — the lunge
// peaks at mid-swing, driven by the same clock the old flipbook used so the
// visual and the hit window cannot desync.
float action_phase01(Action a, const CombatState& cs, const Weapon& w, float sp,
                     const CombatConfig& cfg) {
    float T;
    if (a == Action::Attack)      T = (w.windup + w.active + w.recovery) / sp;
    else if (a == Action::Bomb)   T = (cfg.bomb_cast + cfg.bomb_recover) / sp;
    else if (a == Action::Roll)   T = cfg.roll_duration;
    else if (a == Action::Taunt)  T = (cfg.taunt_cast + cfg.taunt_recover) / sp;
    else if (a == Action::Potion) T = (cfg.potion_cast + cfg.potion_recover) / sp;
    else                          T = cfg.stagger_time;
    T = std::max(0.001f, T);
    return std::min(1.0f, std::max(0.0f, cs.timer / T));
}

// PaperAnimationSystem (플립북 폐지, 2026-07-08) — 정지 방향 포즈 + 수학 트랜스폼.
// facing→(포즈 열, 좌우 플립)은 모든 스프라이트에 적용(단일셀 프롭은 열 0 클램프라
// 무영향, 다중셀 캐릭터/주민은 4포즈로 카메라를 바라봄). 이동 홉·공격 런지·피격
// 흔들림·죽음 붕괴 트랜스폼은 Character에만. 전부 시각 전용 — 물리 무접촉.
// (구 SpriteAnimationSystem + SpriteFacingSystem 대체.)
void PaperAnimationSystem(Arimu::Query<Transform, BillboardSprite> sprites,
                          Arimu::Query<Character> chars,
                          Arimu::Query<Locomotion> locos,
                          Arimu::Query<CombatState> states,
                          Arimu::Query<Weapon> weapons,
                          Arimu::Query<Speed> speeds,
                          Arimu::Res<CombatConfig> cfg,
                          Arimu::Res<FrameTime> time,
                          Arimu::Res<OrbitCamera> cam) {
    const float dt = time->dt;
    const Float3 cp = cam->position();
    for (auto [e, tf, bb] : sprites.each()) {
        if (!bb.sheet.valid || bb.sheet.frame_count <= 0) continue;

        // reset this frame's visual transform
        bb.flip_x = false;
        bb.bob_y = 0.0f; bb.off_x = 0.0f; bb.off_z = 0.0f;
        bb.scale_x = 1.0f; bb.scale_y = 1.0f;
        bb.anim_time += dt;
        bb.cur_dir = 0;

        // facing -> pose column (+ flip). Single-cell props clamp to column 0.
        const float dx_ = cp.x - tf.position.x;
        const float dz_ = cp.z - tf.position.z;
        const float yaw_to_cam = std::atan2(dx_, dz_) * kRad2Deg;
        const paper::PoseFlip pf = paper::pose_flip_of(yaw_to_cam - tf.yaw_deg);
        bb.cur_frame = std::min(pf.pose, bb.sheet.frame_count - 1);
        bb.flip_x = (bb.sheet.frame_count > 1) ? pf.flip : false;

        // procedural transforms: living characters only
        if (!chars.Contains(e)) continue;

        Action act = Action::Idle;
        if (states.Contains(e)) act = states.Get<CombatState>(e).action;

        if (act == Action::Dead) {
            bb.death_t = std::min(1.0f, bb.death_t + dt / 0.6f);   // collapse over ~0.6s
            bb.bob_y = paper::death_sink_of(bb.death_t);
            const paper::Squash sq = paper::death_squash_of(bb.death_t);
            bb.scale_x = sq.sx; bb.scale_y = sq.sy;
            bb.hop_dist = 0.0f;
        } else {
            bb.death_t = 0.0f;
            if (lunges(act) && weapons.Contains(e) && states.Contains(e)) {
                const CombatState& cs = states.Get<CombatState>(e);
                const Weapon& w = weapons.Get<Weapon>(e);
                const float sp = speeds.Contains(e) ? std::max(0.1f, speeds.Get<Speed>(e).v) : 1.0f;
                const float lunge = paper::lunge_of(action_phase01(act, cs, w, sp, *cfg));
                const float yr = tf.yaw_deg * kDeg2Rad;   // 유닛 facing 방향으로 전진
                bb.off_x = lunge * std::sin(yr);
                bb.off_z = lunge * std::cos(yr);
                bb.hop_dist = 0.0f;
            } else if (act == Action::Idle && locos.Contains(e) &&
                       locos.Get<Locomotion>(e).move == Locomotion::Move::Walk) {
                bb.hop_dist += locos.Get<Locomotion>(e).speed * dt;   // 거리 위상 홉
                bb.bob_y = paper::bob_of(bb.hop_dist);
                const paper::Squash sq = paper::squash_of(bb.bob_y);
                bb.scale_x = sq.sx; bb.scale_y = sq.sy;
            } else if (act == Action::Idle) {
                bb.hop_dist = 0.0f;
                bb.scale_y = paper::idle_breathe(bb.anim_time);   // 미세 숨쉬기
            } else {
                bb.hop_dist = 0.0f;   // casts/stances (Taunt/Potion/Guard/Aim/Stagger): pose only
            }
        }

        // hit shake overlay (screen-horizontal = camera-right in XZ)
        if (states.Contains(e)) {
            const CombatState& st = states.Get<CombatState>(e);
            if (st.flash_timer > 0.0f && act != Action::Dead) {
                const float norm = std::min(1.0f, st.flash_timer / 0.18f);
                const float s = paper::shake_of(norm, bb.anim_time);
                const float len = std::sqrt(dx_ * dx_ + dz_ * dz_);
                if (len > 1e-3f) {
                    bb.off_x += s * (dz_ / len);
                    bb.off_z += s * (-dx_ / len);
                }
            }
        }
    }
}

} // namespace

void SpritePlugin::Build(Arimu::App& app) {
    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(SpriteLoadSystem, scene, Arimu::Phase::PreLogic, "SpriteLoad");
    app.AddSystem(LocomotionSystem, scene, Arimu::Phase::Logic, "Locomotion");
    app.AddSystem(PaperAnimationSystem, scene, Arimu::Phase::Logic, "PaperAnimation");
}

} // namespace hd2d
