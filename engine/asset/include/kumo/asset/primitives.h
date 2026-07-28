#pragma once

#include <kumo/asset/asset.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace kumo::asset {

// Front faces wind counter-clockwise seen from outside (FrontFace::CCW + CullMode::Back).
// Out-of-range arguments are clamped rather than rejected, so these never fail.
MeshData makeSphere(float radius = 0.5f, std::uint32_t segments = 32, std::uint32_t rings = 16);
MeshData makeCube(float halfExtent = 0.5f);
MeshData makePlane(float halfExtent = 0.5f, std::uint32_t subdivisions = 1);
// Y-axis aligned, centered at origin, capped top and bottom.
MeshData makeCylinder(float radius = 0.25f, float halfHeight = 0.5f, std::uint32_t segments = 32);
// Y-axis aligned, apex at +halfHeight, capped base at -halfHeight.
MeshData makeCone(float radius = 0.25f, float halfHeight = 0.5f, std::uint32_t segments = 32);
// XZ-plane ring around Y; `minorRadius` is clamped below `majorRadius` (no self-intersection).
MeshData makeTorus(float majorRadius = 0.35f, float minorRadius = 0.15f,
                   std::uint32_t segments = 32, std::uint32_t rings = 16);
// Cylinder body with hemispherical caps sharing the body's rim rings (no seam).
MeshData makeCapsule(float radius = 0.25f, float halfHeight = 0.25f, std::uint32_t segments = 32,
                     std::uint32_t rings = 8);

// Name-based entry shared by the agent tool and scene-load rebuild so both map
// a saved primitive name + size to the same geometry. `size` is the overall
// extent; nullopt for an unknown name.
//
// Mapping (these ratios ARE the on-disk/tool format; changing them breaks
// existing saved scenes):
//   sphere:   makeSphere(size*0.5)
//   cube:     makeCube(size*0.5)
//   plane:    makePlane(size*0.5)
//   cylinder: makeCylinder(size*0.25, size*0.5)
//   cone:     makeCone(size*0.25, size*0.5)
//   torus:    makeTorus(size*0.375, size*0.125)
//   capsule:  makeCapsule(size*0.25, size*0.25)   // total height = size, caps included
std::optional<MeshData> makePrimitive(std::string_view name, float size);

} // namespace kumo::asset
