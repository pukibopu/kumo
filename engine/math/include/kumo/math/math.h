#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace kumo::math {

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using float3x3 = glm::mat3;
using float4x4 = glm::mat4;
using quat = glm::quat;

using glm::cross;
using glm::dot;
using glm::inverse;
using glm::length;
using glm::normalize;
using glm::radians;
using glm::transpose;

// World space is right-handed, +Y up, camera looking down -Z. Clip depth is [0, 1].
inline float4x4 lookAt(const float3& eye, const float3& center, const float3& up) {
    return glm::lookAtRH(eye, center, up);
}

// Reversed-Z: near plane maps to depth 1, far to 0, which evens out float precision.
inline float4x4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    return glm::perspectiveRH_ZO(fovYRadians, aspect, farZ, nearZ);
}

// Reversed-Z with the far plane at infinity: near -> depth 1, z -> -inf -> depth 0.
inline float4x4 perspectiveInfinite(float fovYRadians, float aspect, float nearZ) {
    float f = 1.0f / std::tan(fovYRadians * 0.5f);
    float4x4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;
    m[3][2] = nearZ;
    return m;
}

// Quaternion orienting -Z onto `forward`, right-handed (matches the camera convention).
// Falls back to a stable up axis when `up` is parallel to `forward`.
inline quat quatLookAt(const float3& forward, const float3& up) {
    float len = length(forward);
    float3 f = len > 1e-6f ? forward / len : float3(0.0f, 0.0f, -1.0f);
    float3 u = normalize(up);
    if (std::abs(dot(f, u)) > 0.9999f) {
        u = std::abs(f.y) > 0.9999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    }
    return glm::quatLookAtRH(f, u);
}

inline float4x4 trs(const float3& translation, const quat& rotation, const float3& scale) {
    return glm::translate(float4x4(1.0f), translation) * glm::mat4_cast(rotation) *
           glm::scale(float4x4(1.0f), scale);
}

struct Trs {
    float3 translation{0.0f};
    quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float3 scale{1.0f};
};

// Decomposes a TRS matrix (no shear or negative scale; glTF node transforms
// flattened to world space satisfy this).
inline Trs decomposeTrs(const float4x4& m) {
    Trs out;
    out.translation = float3(m[3]);
    const float3 c0(m[0]);
    const float3 c1(m[1]);
    const float3 c2(m[2]);
    out.scale = {length(c0), length(c1), length(c2)};
    const float3 safe{out.scale.x > 1e-8f ? out.scale.x : 1.0f,
                      out.scale.y > 1e-8f ? out.scale.y : 1.0f,
                      out.scale.z > 1e-8f ? out.scale.z : 1.0f};
    out.rotation = glm::quat_cast(float3x3(c0 / safe.x, c1 / safe.y, c2 / safe.z));
    return out;
}

} // namespace kumo::math
