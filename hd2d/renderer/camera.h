#pragma once

#include "core/math_compat.h"

namespace hd2d {

// Fixed-pitch orbit camera (HD-2D / Octopath style). The camera orbits a target
// point on the ground: pitch is LOCKED (you can never look up/down), only yaw
// (left/right rotation), zoom (distance), and target panning are allowed. This
// pairs with Y-axis billboards so sprites always read as upright characters.
class OrbitCamera {
public:
    // Tunables (exposed in the ImGui panel so the look can be dialed in live).
    float pitch_deg = 15.0f;   // 낮은 백뷰(2026-06-21, 사용자: "훨씬 낮춰 백뷰처럼").
                               // 15° 기준 ±7°(8~22)로만 — 적 프레이밍은 pitch가 아니라 yaw로.
    float fov_deg = 45.0f;
    float near_z = 0.1f;
    float far_z = 2500.0f;  // covers the overworld horizon (entrances sit 5.4km
                            // apart; you still only ever SEE a local patch)
    float min_distance = 3.0f;
    float max_distance = 40.0f;
    float move_speed = 6.0f;   // world units / second (WASD pans the target)
    float rot_speed = 90.0f;        // degrees / second (Q/E keyboard rotate)
    float mouse_sensitivity = 0.2f; // degrees / pixel of mouse motion (look)
    float zoom_step = 1.5f;         // distance / wheel tick

    void update(float mouse_dx, float wheel_ticks, float dt);

    // Build matrices. Call set_aspect() (or pass aspect) before reading proj.
    dx::XMMATRIX view() const;
    dx::XMMATRIX proj(float aspect) const;

    Float3 position() const { return pos_; }
    Float3 target() const { return target_; }
    float distance() const { return distance_; }
    float yaw_deg() const { return yaw_deg_; }

    void set_target(const Float3& t) { target_ = t; }
    void set_distance(float d) { distance_ = d; }
    void set_yaw(float deg) { yaw_deg_ = deg; }

    // Directly place the eye (bypasses the orbit recompute in update()). Used by
    // the encounter/shoulder camera, which scripts pos + target itself.
    void set_position(const Float3& p) { pos_ = p; }

private:
    Float3 target_ = {0.0f, 0.5f, 0.0f};
    Float3 pos_ = {0.0f, 5.0f, 10.0f};
    float yaw_deg_ = 0.0f;
    float distance_ = 12.0f;
};

} // namespace hd2d
