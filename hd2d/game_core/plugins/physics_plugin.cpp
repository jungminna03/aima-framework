#include "game_core/plugins/physics_plugin.h"

#include "core/log_compat.h"
#include "game/combat_components.h"
#include "game/components.h"
#include "game/physics_components.h"
#include "game/platform.h"           // sys::env (PAL env wrapper)
#include "game/resources.h"
#include "game/scene_enum.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>   // AllHitCollisionCollector
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace hd2d {

namespace {

// ---------------------------------------------------------------------------
// Layers: static world vs everything that moves (constants in the header).
// ---------------------------------------------------------------------------
namespace Layers = PhysLayers;

namespace BPLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint NUM_LAYERS = 2;
} // namespace BPLayers

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BPLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        if (layer1 == Layers::NON_MOVING) return layer2 == BPLayers::MOVING;
        return true;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true;   // moving collides with everything
    }
};

constexpr float kMaxStepDt = 1.0f / 20.0f;
constexpr float kGravity = -9.81f;

JPH::Vec3 to_jph(const Float3& v) { return JPH::Vec3(v.x, v.y, v.z); }

// ---------------------------------------------------------------------------
// Body builders from Blender extras (spec §1.2 / §4).
// ---------------------------------------------------------------------------

// World-space copy of a primitive's positions.
std::vector<JPH::Vec3> world_positions(const CpuMesh& cpu, const math::Mat4x4& w) {
    std::vector<JPH::Vec3> out;
    out.reserve(cpu.positions.size());
    for (const Float3& p : cpu.positions) {
        out.emplace_back(p.x * w.m[0][0] + p.y * w.m[1][0] + p.z * w.m[2][0] + w.m[3][0],
                         p.x * w.m[0][1] + p.y * w.m[1][1] + p.z * w.m[2][1] + w.m[3][1],
                         p.x * w.m[0][2] + p.y * w.m[1][2] + p.z * w.m[2][2] + w.m[3][2]);
    }
    return out;
}

// Static collider: geometry baked to world space, body at the origin.
JPH::Ref<JPH::Shape> build_static_shape(const MeshRenderer& mr, const math::Mat4x4& w,
                                        const std::string& shape) {
    if (shape == "box" || shape == "sphere" || shape == "convex") {
        std::vector<JPH::Vec3> pts;
        for (const LoadedPrimitive& prim : mr.prims) {
            auto wp = world_positions(prim.cpu, w);
            pts.insert(pts.end(), wp.begin(), wp.end());
        }
        if (pts.empty()) return nullptr;
        if (shape == "convex") {
            JPH::ConvexHullShapeSettings s(pts.data(), static_cast<int>(pts.size()));
            auto result = s.Create();
            return result.IsValid() ? result.Get() : nullptr;
        }
        JPH::Vec3 mn = pts[0], mx = pts[0];
        for (const auto& p : pts) {
            mn = JPH::Vec3::sMin(mn, p);
            mx = JPH::Vec3::sMax(mx, p);
        }
        const JPH::Vec3 center = 0.5f * (mn + mx);
        if (shape == "sphere") {
            float r = 0.01f;
            for (const auto& p : pts) r = std::max(r, (p - center).Length());
            auto result = JPH::RotatedTranslatedShapeSettings(
                              center, JPH::Quat::sIdentity(),
                              new JPH::SphereShape(r))
                              .Create();
            return result.IsValid() ? result.Get() : nullptr;
        }
        const JPH::Vec3 he = JPH::Vec3::sMax(0.5f * (mx - mn), JPH::Vec3::sReplicate(0.01f));
        auto result = JPH::RotatedTranslatedShapeSettings(center, JPH::Quat::sIdentity(),
                                                          new JPH::BoxShape(he))
                          .Create();
        return result.IsValid() ? result.Get() : nullptr;
    }

    // Default: exact triangle mesh.
    JPH::VertexList verts;
    JPH::IndexedTriangleList tris;
    for (const LoadedPrimitive& prim : mr.prims) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        auto wp = world_positions(prim.cpu, w);
        for (const auto& p : wp)
            verts.push_back(JPH::Float3(p.GetX(), p.GetY(), p.GetZ()));
        for (size_t i = 0; i + 2 < prim.cpu.indices.size(); i += 3) {
            tris.push_back(JPH::IndexedTriangle(base + prim.cpu.indices[i],
                                                base + prim.cpu.indices[i + 1],
                                                base + prim.cpu.indices[i + 2]));
        }
    }
    if (tris.empty()) return nullptr;
    auto result = JPH::MeshShapeSettings(verts, tris).Create();
    return result.IsValid() ? result.Get() : nullptr;
}

// Dynamic collider: LOCAL shape (scale baked) so the body can move/rotate.
JPH::Ref<JPH::Shape> build_dynamic_shape(const MeshRenderer& mr, const math::Mat4x4& w,
                                         const std::string& shape) {
    // Bake node scale into local points (basis row lengths).
    const float sx = std::sqrt(w.m[0][0] * w.m[0][0] + w.m[0][1] * w.m[0][1] +
                               w.m[0][2] * w.m[0][2]);
    const float sy = std::sqrt(w.m[1][0] * w.m[1][0] + w.m[1][1] * w.m[1][1] +
                               w.m[1][2] * w.m[1][2]);
    const float sz = std::sqrt(w.m[2][0] * w.m[2][0] + w.m[2][1] * w.m[2][1] +
                               w.m[2][2] * w.m[2][2]);
    std::vector<JPH::Vec3> pts;
    for (const LoadedPrimitive& prim : mr.prims)
        for (const Float3& p : prim.cpu.positions)
            pts.emplace_back(p.x * sx, p.y * sy, p.z * sz);
    if (pts.empty()) return nullptr;

    if (shape == "sphere") {
        float r = 0.01f;
        for (const auto& p : pts) r = std::max(r, p.Length());
        return new JPH::SphereShape(r);
    }
    if (shape == "box" || shape.empty()) {
        JPH::Vec3 mn = pts[0], mx = pts[0];
        for (const auto& p : pts) {
            mn = JPH::Vec3::sMin(mn, p);
            mx = JPH::Vec3::sMax(mx, p);
        }
        const JPH::Vec3 he = JPH::Vec3::sMax(0.5f * (mx - mn), JPH::Vec3::sReplicate(0.01f));
        const JPH::Vec3 center = 0.5f * (mn + mx);
        if (shape == "box" && center.Length() > 1e-4f) {
            auto result = JPH::RotatedTranslatedShapeSettings(center, JPH::Quat::sIdentity(),
                                                              new JPH::BoxShape(he))
                              .Create();
            return result.IsValid() ? result.Get() : nullptr;
        }
        if (shape == "box") return new JPH::BoxShape(he);
    }
    // convex (and the dynamic default).
    JPH::ConvexHullShapeSettings s(pts.data(), static_cast<int>(pts.size()));
    auto result = s.Create();
    return result.IsValid() ? result.Get() : nullptr;
}

// The static-world surface height straight below/above a point (ray cast over a
// tall vertical scan window). Used to rescue BURIED character positions: a map
// without a spawn="player" empty can place a spawn inside the terrain mesh, and
// a buried CharacterVirtual can never resolve a move — it freezes at the spawn
// forever. Returns true + the surface y when the static world is hit.
bool static_ground_height(JPH::PhysicsSystem& ps, const JPH::Vec3& p, float& y_out) {
    // 탐침 시작 = 머리 위 2m(2026-07-06 다층 인테리어 수정): 예전엔 +100m부터 쏴서
    // 통합 여관의 2층 바닥이 첫 히트 — 1층 스폰(플레이어/두루)이 2층으로 끌려 올라갔다.
    // 얕은 매몰 구조(경사 스폰)는 2m 안에서 충분히 구조된다.
    constexpr float kUp = 2.0f;
    constexpr float kDown = 202.0f;
    const JPH::RRayCast ray(JPH::RVec3(p.GetX(), p.GetY() + kUp, p.GetZ()),
                            JPH::Vec3(0.0f, -kDown, 0.0f));
    JPH::RayCastResult hit;
    if (!ps.GetNarrowPhaseQuery().CastRay(
            ray, hit, JPH::SpecifiedBroadPhaseLayerFilter(BPLayers::NON_MOVING),
            JPH::SpecifiedObjectLayerFilter(Layers::NON_MOVING)))
        return false;
    y_out = p.GetY() + kUp - kDown * hit.mFraction;
    return true;
}

// Lift a desired character position ONTO the surface when it is below it (never
// pushes down — falling onto the ground is gravity's job). Tiny epsilon avoids
// spawning in exact penetration.
JPH::Vec3 unbury(JPH::PhysicsSystem& ps, const JPH::Vec3& p) {
    float ground = 0.0f;
    if (static_ground_height(ps, p, ground) && ground > p.GetY())
        return JPH::Vec3(p.GetX(), ground + 0.02f, p.GetZ());
    return p;
}

// Engine-space rotation quaternion from a world matrix (orthonormalized).
JPH::Quat rotation_of(const math::Mat4x4& w) {
    math::Vec scale, rot, trans;
    const math::Matrix m = math::load(w);
    if (!math::decompose(m, scale, rot, trans)) return JPH::Quat::sIdentity();
    math::Vec4 q;
    math::store4(q, rot);
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

// Build Jolt bodies for freshly-spawned map entities (initial spawn AND every
// hot-reload respawn): default = static mesh collision; extras opt into
// dynamic bodies or out of physics entirely. Runs before the step.
void MapBodySpawnSystem(Arimu::Query<MapEntity, MeshRenderer, WorldMatrix, NodeExtras> nodes,
                        Arimu::Query<StaticBody> statics,
                        Arimu::Query<RigidBody> rigids,
                        Arimu::ResMut<Physics> phys,
                        Arimu::Commands cmd) {
    if (!phys->ready) return;
    JPH::BodyInterface& bi = phys->system->GetBodyInterface();

    for (auto [e, mr, wm, ex] : nodes.each()) {   // MapEntity is an empty tag
        if (statics.Contains(e) || rigids.Contains(e)) continue;   // already built
        if (ex.cloth || ex.body == "none") continue;

        if (ex.body == "dynamic") {
            JPH::Ref<JPH::Shape> shape = build_dynamic_shape(mr, wm.m, ex.shape);
            if (!shape) continue;
            const JPH::Vec3 pos(wm.m.m[3][0], wm.m.m[3][1], wm.m.m[3][2]);
            JPH::BodyCreationSettings bcs(shape.GetPtr(), pos, rotation_of(wm.m),
                                          JPH::EMotionType::Dynamic, Layers::MOVING);
            bcs.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = std::max(0.01f, ex.mass);
            bcs.mFriction = ex.friction;
            bcs.mRestitution = ex.restitution;
            const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
            cmd.AddComponent<RigidBody>(e, RigidBody{id});
            cmd.AddComponent<Rotation3D>(e, Rotation3D{});
        } else {
            JPH::Ref<JPH::Shape> shape = build_static_shape(mr, wm.m, ex.shape);
            if (!shape) continue;
            JPH::BodyCreationSettings bcs(shape.GetPtr(), JPH::Vec3::sZero(),
                                          JPH::Quat::sIdentity(),
                                          JPH::EMotionType::Static, Layers::NON_MOVING);
            bcs.mFriction = ex.friction;
            bcs.mRestitution = ex.restitution;
            // Carry the owning entity so a camera->player raycast (OcclusionDetect)
            // can map a hit static body back to the mesh entity to fade.
            bcs.mUserData = static_cast<JPH::uint64>(e);
            const JPH::BodyID id =
                bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
            cmd.AddComponent<StaticBody>(e, StaticBody{id});
        }
    }
}

// Camera-occlusion fade (user 2026-06-22): raycast camera -> player chest each
// frame; every static map body the ray crosses is "occluding" and fades to
// translucent so the character stays visible. The render side reads OcclusionState.
// The ray ends at chest height, so the ground (a static body too) is never hit.
void OcclusionDetectSystem(Arimu::Res<OrbitCamera> cam,
                           Arimu::Query<Controlled, Transform> players,
                           Arimu::Query<MeshRenderer> meshes,
                           Arimu::Res<MapScene> map,
                           Arimu::ResMut<Physics> phys,
                           Arimu::ResMut<OcclusionState> occ,
                           Arimu::Res<FrameTime> time) {
    constexpr float kFadeAlpha = 0.30f;   // how transparent an occluder becomes
    constexpr float kChest     = 0.8f;    // aim above the feet so the ground is missed
    constexpr float kSpringBuffer = 0.4f; // 벽 바로 앞에서 멈추는 여유(카메라가 벽에 딱 붙지 않게)
    constexpr float kSpringMin    = 3.0f; // OrbitCamera.min_distance와 일치(이보다 더 못 당김)

    // 히트 정적 바디를 벽('wall' 재질)/프롭으로 분류(설계 2026-07-09): 엔티티 → MeshRenderer →
    // prims[i].material → map->materials[idx].name. 벽은 페이드 대신 스프링암(카메라 당김), 프롭은
    // 기존 반투명 페이드. 재질 이름 방식이라 호스트 로더 무손질(GLB 재생성만). 인테리어 맵에만
    // 'wall' 재질이 있어 자연히 인테리어 한정(아웃도어 벽은 이름이 달라 프롭 취급 = 기존 페이드).
    auto is_wall = [&](uint32_t e) -> bool {
        const entt::entity ent = static_cast<entt::entity>(e);
        if (!meshes.Contains(ent)) return false;
        const MeshRenderer& mr = meshes.Get<MeshRenderer>(ent);
        for (const LoadedPrimitive& p : mr.prims)
            if (p.material >= 0 && p.material < static_cast<int>(map->materials.size()) &&
                map->materials[p.material].name == "wall")
                return true;
        return false;
    };

    std::vector<uint32_t> occluding;
    float nearest_wall_pivot = -1.0f;     // 플레이어(오빗 타깃 근사)에서 가장 가까운 벽까지 거리(<0=없음)
    if (phys->ready) {
        Float3 pp{}; bool have = false;
        for (auto [e, tf] : players.each()) { pp = tf.position; have = true; break; }
        if (have) {
            const Float3 cp = cam->position();
            const JPH::Vec3 dir(pp.x - cp.x, (pp.y + kChest) - cp.y, pp.z - cp.z);
            if (dir.LengthSq() > 0.04f) {   // span = cam -> chest (a finite segment)
                const float ray_len = std::sqrt(dir.LengthSq());
                const JPH::RRayCast ray(JPH::RVec3(cp.x, cp.y, cp.z), dir);
                JPH::RayCastSettings rs;
                // Map meshes carry the glTF->DX winding flip, so the camera-facing
                // wall triangles are back-faces in Jolt — collide with both sides.
                rs.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
                rs.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
                JPH::AllHitCollisionCollector<JPH::CastRayCollector> hits;
                phys->system->GetNarrowPhaseQuery().CastRay(
                    ray, rs, hits,
                    JPH::SpecifiedBroadPhaseLayerFilter(BPLayers::NON_MOVING),
                    JPH::SpecifiedObjectLayerFilter(Layers::NON_MOVING));
                JPH::BodyInterface& bi = phys->system->GetBodyInterface();
                for (const auto& h : hits.mHits) {
                    const uint32_t e = static_cast<uint32_t>(bi.GetUserData(h.mBodyID));
                    if (is_wall(e)) {
                        // 벽: 페이드 안 함. 플레이어에서 이 벽까지 거리 = ray_len*(1-fraction);
                        // 가장 가까운 벽(최소)이 카메라를 가장 많이 당긴다(모든 벽을 화면 밖으로).
                        const float pivot_d = ray_len * (1.0f - h.mFraction);
                        if (nearest_wall_pivot < 0.0f || pivot_d < nearest_wall_pivot)
                            nearest_wall_pivot = pivot_d;
                    } else {
                        occluding.push_back(e);   // 프롭/기타 정적 바디: 반투명 페이드(기존)
                    }
                }
                if (sys::env("HD2D_OCCDEBUG")) {
                    static int s_n = 0;
                    if ((s_n++ % 60) == 0)
                        HD2D_INFO("[occdbg] ready={} cam=({:.1f},{:.1f},{:.1f}) "
                                  "ply=({:.1f},{:.1f},{:.1f}) len={:.1f} hits={} props={} wall_pivot={:.2f}",
                                  phys->ready, cp.x, cp.y, cp.z, pp.x, pp.y, pp.z,
                                  ray_len, hits.mHits.size(), occluding.size(), nearest_wall_pivot);
                }
            }
        }
    }

    // 스프링암 목표 오빗 거리: 벽 있으면 (가장 가까운 벽 - 버퍼), min_distance 이상으로 클램프;
    // 없으면 <0 → 카메라는 인테리어 기본 거리로 복귀(CameraOrbitSystem 인테리어 블록이 소비).
    // 프롭 페이드와 독립 — 같은 프레임에 벽 스프링 + 프롭 페이드가 동시 성립.
    occ->wall_spring = (nearest_wall_pivot >= 0.0f)
                           ? std::max(kSpringMin, nearest_wall_pivot - kSpringBuffer)
                           : -1.0f;

    auto is_occluding = [&](uint32_t e) {
        return std::find(occluding.begin(), occluding.end(), e) != occluding.end();
    };

    // New occluders enter at fully-opaque so they fade IN (no pop).
    for (uint32_t e : occluding)
        if (occ->alpha.find(e) == occ->alpha.end()) occ->alpha[e] = 1.0f;

    // Lerp every tracked entity toward its target; drop ones that returned to opaque.
    const float k = std::min(1.0f, 12.0f * time->dt);   // ~0.12s time-constant
    for (auto it = occ->alpha.begin(); it != occ->alpha.end(); ) {
        const float target = is_occluding(it->first) ? kFadeAlpha : 1.0f;
        it->second += (target - it->second) * k;
        if (target >= 1.0f && it->second > 0.995f) it = occ->alpha.erase(it);
        else ++it;
    }

    // Edge-triggered log for the headless regression (HD2D_OCCLUDETEST).
    static size_t s_prev = 0;
    if (occluding.size() != s_prev) {
        HD2D_INFO("[occlude] {} mesh(es) between camera and player (fade -> {:.2f})",
                  occluding.size(), kFadeAlpha);
        s_prev = occluding.size();
    }
}

// Interpret gameplay's Transform writes as desired movement, resolve them
// through CharacterVirtual (walls/slopes/steps/gravity), write back, and keep
// the kinematic mirror capsule in sync for dynamics & cloth.
void CharacterPhysicsSystem(Arimu::Query<Character, Transform> characters,
                            Arimu::Query<CharacterBody> bodies,
                            Arimu::Query<BillboardSprite> sprites,
                            Arimu::Res<RenderSettings> rs,
                            Arimu::Res<FrameTime> time,
                            Arimu::ResMut<Physics> phys,
                            Arimu::Commands cmd) {
    if (!phys->ready) return;
    const float dt = std::min(std::max(time->dt, 1e-4f), kMaxStepDt);
    JPH::PhysicsSystem& ps = *phys->system;
    JPH::BodyInterface& bi = ps.GetBodyInterface();

    for (auto [e, tf] : characters.each()) {   // Character is an empty tag
        if (!bodies.Contains(e)) {
            // Lazy create: a capsule whose position = the FEET. Height follows
            // the unit convention (32px = 1m): sprite frame_px / pixels_per_unit,
            // so render height and collision height agree for any sheet size.
            float total = 1.0f;                             // no sprite: 1m default
            if (sprites.Contains(e)) {
                const SpriteSheet& sheet = sprites.Get<BillboardSprite>(e).sheet;
                if (!sheet.valid) continue;  // sheet still loading: wait a frame
                total = static_cast<float>(sheet.frame_px) / rs->pixels_per_unit;
            }
            // Rescue a buried spawn BEFORE the capsule exists: a character placed
            // inside the static world would otherwise freeze there forever.
            {
                const JPH::Vec3 safe = unbury(ps, to_jph(tf.position));
                if (safe.GetY() != tf.position.y) {
                    HD2D_INFO("[phys] lifted buried character {} {:.2f} -> {:.2f}",
                              static_cast<uint32_t>(e), tf.position.y, safe.GetY());
                    tf.position.y = safe.GetY();
                }
            }
            CharacterBody cb;
            cb.radius = 0.28f * total;   // keep the capsule proportions per height
            cb.half_height = std::max(0.05f, 0.5f * total - cb.radius);
            JPH::Ref<JPH::CharacterVirtualSettings> settings =
                new JPH::CharacterVirtualSettings();
            settings->mShape =
                JPH::RotatedTranslatedShapeSettings(
                    JPH::Vec3(0, cb.half_height + cb.radius, 0), JPH::Quat::sIdentity(),
                    new JPH::CapsuleShape(cb.half_height, cb.radius))
                    .Create()
                    .Get();
            settings->mSupportingVolume =
                JPH::Plane(JPH::Vec3::sAxisY(), -cb.radius * 0.9f);
            cb.character = new JPH::CharacterVirtual(
                settings.GetPtr(), to_jph(tf.position), JPH::Quat::sIdentity(), 0, &ps);

            // Kinematic mirror so rigid bodies / cloth collide with characters.
            JPH::BodyCreationSettings mirror(
                settings->mShape.GetPtr(), to_jph(tf.position), JPH::Quat::sIdentity(),
                JPH::EMotionType::Kinematic, Layers::MOVING);
            cb.mirror = bi.CreateAndAddBody(mirror, JPH::EActivation::Activate);
            cmd.AddComponent<CharacterBody>(e, cb);
            continue;   // starts resolving next frame
        }

        CharacterBody& cb = bodies.Get<CharacterBody>(e);
        JPH::CharacterVirtual& ch = *cb.character;
        const JPH::Vec3 current = ch.GetPosition();
        const JPH::Vec3 desired = to_jph(tf.position);
        const JPH::Vec3 delta = desired - current;

        // Teleports (respawn, scripted tests) snap instead of flinging — lifted
        // out of the static world first so a buried target can't wedge us.
        if (delta.Length() > 3.0f) {
            const JPH::Vec3 safe = unbury(ps, desired);
            ch.SetPosition(safe);
            tf.position = Float3{safe.GetX(), safe.GetY(), safe.GetZ()};
            cb.vertical_vel = 0.0f;
            bi.SetPositionAndRotation(cb.mirror, safe, JPH::Quat::sIdentity(),
                                      JPH::EActivation::Activate);
            continue;
        }

        cb.vertical_vel += kGravity * dt;
        if (cb.grounded && cb.vertical_vel < 0.0f) cb.vertical_vel = -0.5f;
        const JPH::Vec3 velocity(delta.GetX() / dt, cb.vertical_vel, delta.GetZ() / dt);
        ch.SetLinearVelocity(velocity);

        JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
        JPH::IgnoreSingleBodyFilter ignore_self(cb.mirror);
        ch.ExtendedUpdate(dt, JPH::Vec3(0, kGravity, 0), update_settings,
                          ps.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                          ps.GetDefaultLayerFilter(Layers::MOVING), ignore_self, {},
                          *phys->temp);

        const JPH::Vec3 result = ch.GetPosition();
        tf.position = Float3{result.GetX(), result.GetY(), result.GetZ()};
        cb.grounded =
            ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        if (cb.grounded && cb.vertical_vel < 0.0f) cb.vertical_vel = 0.0f;

        bi.MoveKinematic(cb.mirror, result, JPH::Quat::sIdentity(), dt);
    }
    (void)rs;
}

// Advance the Jolt world once per frame.
void PhysicsStepSystem(Arimu::Res<FrameTime> time, Arimu::ResMut<Physics> phys) {
    if (!phys->ready) return;
    const float dt = std::min(std::max(time->dt, 1e-4f), kMaxStepDt);
    phys->system->Update(dt, 1, phys->temp.get(), phys->jobs.get());
}

// Write simulated rigid-body poses back to the render components.
void SyncRigidBodiesSystem(Arimu::Query<RigidBody, Transform, Rotation3D> rigids,
                           Arimu::ResMut<Physics> phys) {
    if (!phys->ready) return;
    JPH::BodyInterface& bi = phys->system->GetBodyInterface();
    for (auto [e, rb, tf, rot] : rigids.each()) {
        JPH::Vec3 pos;
        JPH::Quat q;
        bi.GetPositionAndRotation(rb.id, pos, q);
        tf.position = Float3{pos.GetX(), pos.GetY(), pos.GetZ()};
        rot.quat = math::Vec4{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    }
}

// HD2D_PHYSTEST: periodically log dynamic body + player state so an unattended
// run can assert "boxes fell and came to rest, characters stay on the floor".
void PhysTestLogSystem(Arimu::Query<RigidBody, Transform> rigids,
                       Arimu::Query<Player, Transform> players,
                       Arimu::Res<Physics> phys) {
    static const bool enabled = sys::env("HD2D_PHYSTEST") != nullptr;
    if (!enabled || !phys->ready) return;
    static int frame = 0;
    if (++frame % 60 != 0) return;
    for (auto [e, rb, tf] : rigids.each()) {
        HD2D_INFO("[phystest] body {} pos=({:.2f},{:.2f},{:.2f})",
                  static_cast<uint32_t>(e), tf.position.x, tf.position.y, tf.position.z);
    }
    for (auto [e, tf] : players.each()) {   // Player is an empty tag
        HD2D_INFO("[phystest] player pos=({:.2f},{:.2f},{:.2f})", tf.position.x,
                  tf.position.y, tf.position.z);
    }
}

} // namespace

void PhysicsPlugin::Build(Arimu::App& app) {
    // Global Jolt runtime (idempotent enough for a single-app process).
    JPH::RegisterDefaultAllocator();
    if (!JPH::Factory::sInstance) {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    if (!app.GetWorld().HasResource<Physics>()) {   // survives code hot-reloads
        Physics phys;
        phys.temp = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
        phys.jobs = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            std::max(1u, std::thread::hardware_concurrency() - 1));
        phys.bp_layers = std::make_unique<BPLayerInterfaceImpl>();
        phys.obj_vs_bp = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
        phys.obj_pairs = std::make_unique<ObjectLayerPairFilterImpl>();
        phys.system = std::make_unique<JPH::PhysicsSystem>();
        phys.system->Init(1024, 0, 1024, 1024, *phys.bp_layers, *phys.obj_vs_bp,
                          *phys.obj_pairs);
        phys.system->SetGravity(JPH::Vec3(0, kGravity, 0));
        phys.ready = true;
        app.GetWorld().InsertResource<Physics>(std::move(phys));
        HD2D_INFO("physics initialized (Jolt {}.{}.{})", JPH_VERSION_MAJOR,
                  JPH_VERSION_MINOR, JPH_VERSION_PATCH);
    }

    app.GetWorld().EnsureResource<OcclusionState>();   // camera-occlusion fade

    const uint8_t scene = AsIndex(GameScene::World);
    app.AddSystem(MapBodySpawnSystem,      scene, Arimu::Phase::Logic, "MapBodySpawn");
    app.AddSystem(CharacterPhysicsSystem,  scene, Arimu::Phase::Logic, "CharacterPhysics");
    app.AddSystem(PhysicsStepSystem,       scene, Arimu::Phase::Logic, "PhysicsStep");
    app.AddSystem(SyncRigidBodiesSystem,   scene, Arimu::Phase::Logic, "SyncRigidBodies");
    app.AddSystem(OcclusionDetectSystem,   scene, Arimu::Phase::Logic, "OcclusionDetect");
    app.AddSystem(PhysTestLogSystem,       scene, Arimu::Phase::Logic, "PhysTestLog");
}

void physics_remove_entity_bodies(Arimu::World& world, entt::entity e) {
    if (!world.HasResource<Physics>()) return;
    Physics& phys = world.GetResource<Physics>();
    if (!phys.ready) return;
    JPH::BodyInterface& bi = phys.system->GetBodyInterface();
    entt::registry& reg = world.Registry();
    if (auto* sb = reg.try_get<StaticBody>(e); sb && !sb->id.IsInvalid()) {
        bi.RemoveBody(sb->id);
        bi.DestroyBody(sb->id);
    }
    if (auto* rb = reg.try_get<RigidBody>(e); rb && !rb->id.IsInvalid()) {
        bi.RemoveBody(rb->id);
        bi.DestroyBody(rb->id);
    }
    if (auto* cb = reg.try_get<CharacterBody>(e); cb && !cb->mirror.IsInvalid()) {
        bi.RemoveBody(cb->mirror);
        bi.DestroyBody(cb->mirror);
    }
    if (auto* cl = reg.try_get<ClothBody>(e); cl && !cl->id.IsInvalid()) {
        bi.RemoveBody(cl->id);
        bi.DestroyBody(cl->id);
    }
}

} // namespace hd2d
