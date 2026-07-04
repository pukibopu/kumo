#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace kumo::math {

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using float3x3 = glm::mat3;
using float4x4 = glm::mat4;
using quat = glm::quat;

using glm::cross;
using glm::dot;
using glm::normalize;
using glm::radians;

// World space is right-handed, +Y up, camera looking down -Z. Clip depth is [0, 1].
inline float4x4 lookAt(const float3& eye, const float3& center, const float3& up) {
    return glm::lookAtRH(eye, center, up);
}

// Reversed-Z: near plane maps to depth 1, far to 0, which evens out float precision.
inline float4x4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    return glm::perspectiveRH_ZO(fovYRadians, aspect, farZ, nearZ);
}

} // namespace kumo::math
