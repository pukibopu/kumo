#include "shadow.h"

#include <algorithm>
#include <limits>

namespace kumo::renderer::detail {

math::float4x4 fitDirectionalShadow(const math::Aabb& sceneBounds, const math::float3& lightDir,
                                    float padding) {
    if (sceneBounds.max.x < sceneBounds.min.x || sceneBounds.max.y < sceneBounds.min.y ||
        sceneBounds.max.z < sceneBounds.min.z) {
        return math::float4x4(1.0f);
    }
    const float dirLen = math::length(lightDir);
    if (dirLen < 1e-6f) {
        return math::float4x4(1.0f);
    }
    const math::float3 forward = lightDir / dirLen;
    const math::float3 center = (sceneBounds.min + sceneBounds.max) * 0.5f;
    const float radius = math::length(sceneBounds.max - sceneBounds.min) * 0.5f;
    if (radius < 1e-6f) {
        return math::float4x4(1.0f);
    }

    math::float3 up{0.0f, 1.0f, 0.0f};
    if (std::abs(math::dot(forward, up)) > 0.9999f) {
        up = math::float3(0.0f, 0.0f, 1.0f);
    }
    const math::float3 eye = center - forward * radius;
    const math::float4x4 view = math::lookAt(eye, center, up);

    math::float3 minV(std::numeric_limits<float>::max());
    math::float3 maxV(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i) {
        const math::float3 corner{(i & 1) != 0 ? sceneBounds.max.x : sceneBounds.min.x,
                                  (i & 2) != 0 ? sceneBounds.max.y : sceneBounds.min.y,
                                  (i & 4) != 0 ? sceneBounds.max.z : sceneBounds.min.z};
        const math::float3 viewPos(view * math::float4(corner, 1.0f));
        minV.x = std::min(minV.x, viewPos.x);
        minV.y = std::min(minV.y, viewPos.y);
        minV.z = std::min(minV.z, viewPos.z);
        maxV.x = std::max(maxV.x, viewPos.x);
        maxV.y = std::max(maxV.y, viewPos.y);
        maxV.z = std::max(maxV.z, viewPos.z);
    }

    const float padX = padding * (maxV.x - minV.x);
    const float padY = padding * (maxV.y - minV.y);
    // RH view looks down -Z: the closest corner (largest view-space z) is the
    // near plane, the farthest (smallest z) is the far plane.
    const math::float4x4 proj = math::orthographic(minV.x - padX, maxV.x + padX, minV.y - padY,
                                                   maxV.y + padY, -maxV.z, -minV.z);
    return proj * view;
}

} // namespace kumo::renderer::detail
