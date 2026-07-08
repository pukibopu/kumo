#pragma once

#include <kumo/math/math.h>

namespace kumo::scene {

struct Camera {
    math::float3 position{0.0f, 0.0f, 3.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float fovY = math::radians(60.0f);
    float nearZ = 0.1f;

    math::float4x4 view() const {
        return math::inverse(math::trs(position, rotation, {1.0f, 1.0f, 1.0f}));
    }
    math::float4x4 projection(float aspect) const {
        return math::perspectiveInfinite(fovY, aspect, nearZ);
    }

    // LLM/tool-friendly setter (ADR 0039): orient the camera at a target.
    void lookAt(const math::float3& target, const math::float3& up = {0.0f, 1.0f, 0.0f}) {
        rotation = math::quatLookAt(target - position, up);
    }
};

} // namespace kumo::scene
