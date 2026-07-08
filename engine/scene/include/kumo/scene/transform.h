#pragma once

#include <kumo/math/math.h>

namespace kumo::scene {

struct Transform {
    math::float3 position{0.0f, 0.0f, 0.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    math::float3 scale{1.0f, 1.0f, 1.0f};

    math::float4x4 matrix() const;
};

} // namespace kumo::scene
