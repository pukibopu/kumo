#pragma once

#include <kumo/asset/asset.h>

#include <cstdint>

namespace kumo::asset {

// Front faces wind counter-clockwise seen from outside (FrontFace::CCW + CullMode::Back).
// Out-of-range arguments are clamped rather than rejected, so these never fail.
MeshData makeSphere(float radius = 0.5f, std::uint32_t segments = 32, std::uint32_t rings = 16);
MeshData makeCube(float halfExtent = 0.5f);
MeshData makePlane(float halfExtent = 0.5f, std::uint32_t subdivisions = 1);

} // namespace kumo::asset
