#include <kumo/facade/orbit_camera.h>

#include <algorithm>
#include <cmath>

namespace kumo::facade {

void OrbitCamera::rotate(float dx, float dy) {
    yaw_ -= dx * 0.005f;
    pitch_ = std::clamp(pitch_ + dy * 0.005f, -1.5f, 1.5f);
}

void OrbitCamera::zoom(float scroll) {
    // The ceiling follows an agent-imported distance so one scroll notch
    // narrows the range smoothly instead of teleporting back to 20.
    distance_ = std::clamp(distance_ * std::exp(-scroll * 0.12f), 0.5f, std::max(20.0f, distance_));
}

void OrbitCamera::apply(scene::Camera& camera) const {
    const math::float3 offset{std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                              std::cos(pitch_) * std::cos(yaw_)};
    camera.position = target_ + offset * distance_;
    camera.lookAt(target_);
}

void OrbitCamera::syncFrom(const scene::Camera& camera) {
    const float len = math::length(camera.position - target_);
    if (len > 1e-4f) {
        distance_ = len;
    }
    const math::float3 forward = camera.rotation * math::float3(0.0f, 0.0f, -1.0f);
    target_ = camera.position + forward * distance_;
    const math::float3 dir = -forward;
    pitch_ = std::clamp(std::asin(std::clamp(dir.y, -1.0f, 1.0f)), -1.5f, 1.5f);
    yaw_ = std::atan2(dir.x, dir.z);
}

} // namespace kumo::facade
