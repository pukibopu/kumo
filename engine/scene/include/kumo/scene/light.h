#pragma once

#include <kumo/math/math.h>

namespace kumo::scene {

enum class LightType { Directional, Point };

struct Light {
    LightType type = LightType::Directional;
    math::float3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    math::float3 position{0.0f, 0.0f, 0.0f};
    math::float3 direction{0.0f, -1.0f, 0.0f};
    float range = 0.0f;
};

} // namespace kumo::scene
