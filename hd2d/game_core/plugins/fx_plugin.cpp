#include "game_core/plugins/fx_plugin.h"

#include "core/log_compat.h"
#include "game_core/components_core.h"
#include "game_core/fx.h"

#include "game/res_path.h"      // res::sprite_path — 소비자 include 경로로 해석(render_plugin 선례)
#include "game/resources.h"     // FrameTime
#include "game/scene_enum.h"    // GameScene / AsIndex

#include <algorithm>
#include <cmath>

namespace hd2d {

namespace fx {

// 공통 스폰 커널: 정의 조회 → Transform+BillboardSprite+EffectFx 엔티티 즉시 생성.
static entt::entity spawn_impl(entt::registry& reg, const char* id, Float3 pos,
                               entt::entity follow) {
    const EffectDef* def = find_effect_def(id);
    if (!def) {
        HD2D_WARN("[fx] unknown id={}", id ? id : "(null)");
        return entt::null;
    }
    const entt::entity e = reg.create();

    Transform tf;
    tf.position = pos;
    tf.position.y += def->height_m;   // 월드 높이 오프셋
    tf.yaw_deg = 0.0f;
    tf.scale = 1.0f;
    reg.emplace<Transform>(e, tf);

    BillboardSprite bb;
    bb.path = res::sprite_path(def->sheet);
    bb.fps  = def->fps;
    reg.emplace<BillboardSprite>(e, std::move(bb));

    EffectFx fxc;
    fxc.def    = def;
    fxc.t      = 0.0f;
    fxc.follow = follow;
    fxc.base_y = def->height_m;
    reg.emplace<EffectFx>(e, fxc);

    HD2D_INFO("[fx] spawn id={} frames={} loop={}", def->id, def->frames, def->loop ? 1 : 0);
    return e;
}

entt::entity spawn(entt::registry& reg, const char* id, Float3 pos) {
    return spawn_impl(reg, id, pos, entt::null);
}

entt::entity spawn_on(entt::registry& reg, const char* id, entt::entity target) {
    Float3 origin{0.0f, 0.0f, 0.0f};
    // 대상 초기 위치를 기준점으로(추적은 FxSystem 이 매 프레임 갱신).
    if (reg.valid(target) && reg.all_of<Transform>(target))
        origin = reg.get<Transform>(target).position;
    return spawn_impl(reg, id, origin, target);
}

} // namespace fx

namespace {

// FxSystem — EffectFx 진행 + 모션/틴트 커브 → BillboardSprite 채널. 원샷은 수명 후 소멸.
//
// ⚠️ 등록 순서(game_api.cpp: FxPlugin 은 CombatPlugin/SpritePlugin *뒤*에 AddPlugin):
//   - CombatTintSystem(combat_plugin, Logic)이 매 프레임 모든 BillboardSprite.tint 를 1
//     로 리셋 후 전투 틴트를 쓴다. EffectFx 엔티티는 combatant 가 아니므로 리셋값(1,1,1)
//     만 받는다 — FxSystem 이 그 *뒤*에 돌아 자기 틴트 커브를 덮어써야 발광이 산다.
//   - PaperAnimationSystem(sprite_plugin, Logic)도 모든 BillboardSprite 의 scale/off/bob/
//     cur_frame 을 리셋·주입한다(fx 엔티티는 Character 가 아니라 facing 포즈열만 씀).
//     FxSystem 이 *뒤*에 돌며 이 채널을 전부 덮어쓰므로 별도 opt-out 불필요(최종 기록자
//     승리 = 최소·정확 메커니즘). 두 시스템 다 Logic 페이즈라 등록순=실행순.
void FxSystem(Arimu::Query<EffectFx, Transform, BillboardSprite> fxq,
              Arimu::Query<Transform> transforms,
              Arimu::Res<FrameTime> time,
              Arimu::Commands cmd) {
    const float dt = time->dt;
    for (auto [e, fxc, tf, bb] : fxq.each()) {
        const EffectDef* def = fxc.def;
        if (!def) { cmd.Destroy(e); continue; }

        // follow 추적: 대상 소멸 시 이펙트도 소멸.
        if (fxc.follow != entt::null) {
            if (!transforms.Contains(fxc.follow)) {
                HD2D_INFO("[fx] done id={} (follow gone)", def->id);
                cmd.Destroy(e);
                continue;
            }
            const Transform& tgt = transforms.Get<Transform>(fxc.follow);
            tf.position.x = tgt.position.x;
            tf.position.z = tgt.position.z;
            tf.position.y = tgt.position.y + fxc.base_y;
        }

        fxc.t += dt;
        const float life = (def->fps > 1e-4f) ? def->frames / def->fps : 0.0f;

        // 원샷 수명 종료 = 소멸.
        if (!def->loop && def->fps > 1e-4f && fxc.t >= life) {
            HD2D_INFO("[fx] done id={}", def->id);
            cmd.Destroy(e);
            continue;
        }

        // 프레임 인덱스. 시트가 로드됐으면 실제 frame_count 로 클램프(작가값과 어긋남 방지).
        int frames = def->frames;
        if (bb.sheet.valid && bb.sheet.frame_count > 0)
            frames = std::min(frames, bb.sheet.frame_count);
        frames = std::max(1, frames);
        const int fi = int(fxc.t * def->fps);
        bb.cur_frame = def->loop ? (fi % frames) : std::min(fi, frames - 1);
        bb.flip_x = false;

        // 수명 정규화 t01(루프는 프레임 위상, 원샷은 [0,1]).
        const float t01 = def->loop ? (life > 1e-4f ? std::fmod(fxc.t, life) / life : 0.0f)
                                    : std::min(1.0f, (life > 1e-4f ? fxc.t / life : 0.0f));

        // scale pop(in) + 소멸 축소(out, 원샷만).
        float s = 1.0f;
        if (def->scale_in > 1e-4f)  s *= std::min(1.0f, fxc.t / def->scale_in);
        if (!def->loop && def->scale_out > 1e-4f)
            s *= std::min(1.0f, std::max(0.0f, (life - fxc.t) / def->scale_out));
        bb.scale_x = s;
        bb.scale_y = s;

        // rise → bob_y, spin → 궤도 off, shake → off.
        bb.bob_y = def->rise_mps * fxc.t;
        bb.off_x = 0.0f;
        bb.off_z = 0.0f;
        if (def->spin_dps != 0.0f) {
            const float ang = def->spin_dps * fxc.t * 0.0174532925f;   // deg→rad
            const float r = 0.15f;
            bb.off_x += r * std::sin(ang);
            bb.off_z += r * std::cos(ang);
        }
        if (def->shake_amp > 1e-4f) {
            bb.off_x += def->shake_amp * std::sin(fxc.t * 90.0f);
            bb.off_z += def->shake_amp * std::sin(fxc.t * 70.0f);
        }

        // 틴트 커브 tint0→tint1(리셋값 1,1,1 위에 덮어쓰기; >1 = HDR 블룸).
        bb.tint_r = def->tint0[0] + (def->tint1[0] - def->tint0[0]) * t01;
        bb.tint_g = def->tint0[1] + (def->tint1[1] - def->tint0[1]) * t01;
        bb.tint_b = def->tint0[2] + (def->tint1[2] - def->tint0[2]) * t01;
    }
}

} // namespace

void FxPlugin::Build(Arimu::App& app) {
    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(FxSystem, scene, Arimu::Phase::Logic, "Fx");
}

} // namespace hd2d
