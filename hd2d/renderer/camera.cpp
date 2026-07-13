#include "renderer/camera.h"

#include <algorithm>
#include <cmath>

namespace hd2d {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
}

void OrbitCamera::update(float mouse_dx, float wheel_ticks, float dt) {
    // --- yaw (left/right rotation only; pitch stays locked) ---
    (void)dt;  // 키보드 회전(rot_speed*dt) 제거로 미사용 (2026-07-06)
    yaw_deg_ += mouse_dx * mouse_sensitivity;  // 3rd-person mouse-look (마우스 룩 전용 — Q/E 키보드 회전 제거 2026-07-06)
    if (yaw_deg_ >= 360.0f) yaw_deg_ -= 360.0f;
    if (yaw_deg_ < 0.0f)    yaw_deg_ += 360.0f;

    // --- zoom (distance), clamped ---
    distance_ -= wheel_ticks * zoom_step;
    distance_ = std::clamp(distance_, min_distance, max_distance);

    // --- derive camera position from fixed pitch + yaw + distance, orbiting the
    //     target (set each frame to the player character's position) ---
    const float yaw = yaw_deg_ * kDeg2Rad;
    const float pitch = pitch_deg * kDeg2Rad;
    const float horiz = std::cos(pitch) * distance_;
    pos_.x = target_.x + std::sin(yaw) * horiz;
    pos_.y = target_.y + std::sin(pitch) * distance_;
    pos_.z = target_.z + std::cos(yaw) * horiz;
}

dx::XMMATRIX OrbitCamera::view() const {
    return dx::XMMatrixLookAtLH(
        dx::XMVectorSet(pos_.x, pos_.y, pos_.z, 1.0f),
        dx::XMVectorSet(target_.x, target_.y, target_.z, 1.0f),
        dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

dx::XMMATRIX OrbitCamera::proj(float aspect) const {
    return dx::XMMatrixPerspectiveFovLH(fov_deg * kDeg2Rad, aspect, near_z, far_z);
}

} // namespace hd2d
