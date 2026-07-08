#include <kumo/scene/camera.h>

namespace kumo::scene {

math::float4x4 Camera::view() const {
    return math::inverse(math::trs(position, rotation, {1.0f, 1.0f, 1.0f}));
}

math::float4x4 Camera::projection(float aspect) const {
    return math::perspectiveInfinite(fovY, aspect, nearZ);
}

void Camera::lookAt(const math::float3& target, const math::float3& up) {
    rotation = math::quatLookAt(target - position, up);
}

} // namespace kumo::scene
