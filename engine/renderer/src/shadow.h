#pragma once

#include <kumo/math/math.h>

namespace kumo::renderer::detail {

// Fits an orthographic light-view-projection matrix to `sceneBounds` for a
// directional light (ADR 0009). `lightDir` points FROM the light, matching
// scene::Light::direction. Returns identity when the bounds or direction are
// degenerate; the caller then disables shadows for the frame.
math::float4x4 fitDirectionalShadow(const math::Aabb& sceneBounds, const math::float3& lightDir,
                                    float padding = 0.05f);

} // namespace kumo::renderer::detail
