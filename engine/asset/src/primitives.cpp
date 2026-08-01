#include <kumo/asset/primitives.h>

#include <algorithm>
#include <cmath>

namespace kumo::asset {
namespace {

constexpr std::uint32_t kMinSegments = 3;
constexpr std::uint32_t kMaxSegments = 256;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinExtent = 1e-4f;

Vertex makeVertex(const math::float3& position, const math::float3& normal,
                  const math::float3& tangent, float u, float v) {
    return {position.x, position.y, position.z, normal.x, normal.y, normal.z,
            tangent.x,  tangent.y,  tangent.z,  1.0f,     u,        v};
}

// Quad corners run (-t,-b), (+t,-b), (+t,+b), (-t,+b) around `center`; with
// b = cross(n, t) the two triangles wind counter-clockwise seen from outside.
void pushQuad(MeshData& mesh, const math::float3& center, const math::float3& normal,
              const math::float3& tangent, float halfExtent) {
    const math::float3 bitangent = math::cross(normal, tangent);
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(
        makeVertex(center + (-tangent - bitangent) * halfExtent, normal, tangent, 0.0f, 1.0f));
    mesh.vertices.push_back(
        makeVertex(center + (tangent - bitangent) * halfExtent, normal, tangent, 1.0f, 1.0f));
    mesh.vertices.push_back(
        makeVertex(center + (tangent + bitangent) * halfExtent, normal, tangent, 1.0f, 0.0f));
    mesh.vertices.push_back(
        makeVertex(center + (-tangent + bitangent) * halfExtent, normal, tangent, 0.0f, 0.0f));
    for (std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
        mesh.indices.push_back(base + offset);
    }
}

// Connects two adjacent rows of `segments + 1` columns (already pushed, each
// row starting at absolute vertex index `topBase`/`bottomBase`) into a quad
// strip, following makeSphere's winding: (topLeft, topRight, bottomLeft) then
// (topRight, bottomRight, bottomLeft). `skipFirst`/`skipSecond` drop the
// triangle that would be degenerate when the top or bottom row collapses to a
// single point (a pole or an apex).
void pushRingBand(MeshData& mesh, std::uint32_t topBase, std::uint32_t bottomBase,
                  std::uint32_t segments, bool skipFirst, bool skipSecond) {
    for (std::uint32_t segment = 0; segment < segments; ++segment) {
        const std::uint32_t topLeft = topBase + segment;
        const std::uint32_t topRight = topLeft + 1;
        const std::uint32_t bottomLeft = bottomBase + segment;
        const std::uint32_t bottomRight = bottomLeft + 1;
        if (!skipFirst) {
            mesh.indices.push_back(topLeft);
            mesh.indices.push_back(topRight);
            mesh.indices.push_back(bottomLeft);
        }
        if (!skipSecond) {
            mesh.indices.push_back(topRight);
            mesh.indices.push_back(bottomRight);
            mesh.indices.push_back(bottomLeft);
        }
    }
}

// Fan of `segments` triangles connecting a center point to a ring of radius
// `ringRadius` at height `y`, flat-shaded with `normal`. Viewed from outside,
// a +Y cap and a -Y cap wind opposite ways around the Y axis for the same
// increasing-phi vertex order, so `reversed` picks the fan's winding.
void pushDisk(MeshData& mesh, float y, float ringRadius, const math::float3& normal, bool reversed,
              std::uint32_t segments) {
    const math::float3 tangent{1.0f, 0.0f, 0.0f};
    const auto centerIndex = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(makeVertex({0.0f, y, 0.0f}, normal, tangent, 0.5f, 0.5f));
    const auto rimBase = centerIndex + 1;
    for (std::uint32_t segment = 0; segment <= segments; ++segment) {
        const float u = static_cast<float>(segment) / static_cast<float>(segments);
        const float phi = u * 2.0f * kPi;
        const math::float3 position{ringRadius * std::cos(phi), y, ringRadius * std::sin(phi)};
        mesh.vertices.push_back(makeVertex(position, normal, tangent, u, 1.0f));
    }
    for (std::uint32_t segment = 0; segment < segments; ++segment) {
        const std::uint32_t a = rimBase + segment;
        const std::uint32_t b = rimBase + segment + 1;
        mesh.indices.push_back(centerIndex);
        mesh.indices.push_back(reversed ? a : b);
        mesh.indices.push_back(reversed ? b : a);
    }
}

} // namespace

MeshData makeSphere(float radius, std::uint32_t segments, std::uint32_t rings) {
    radius = std::max(radius, kMinExtent);
    segments = std::clamp(segments, 3u, 256u);
    rings = std::clamp(rings, 2u, 128u);

    MeshData mesh;
    mesh.vertices.reserve(static_cast<std::size_t>(segments + 1) * (rings + 1));
    for (std::uint32_t ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);
            const math::float3 normal{sinTheta * cosPhi, cosTheta, sinTheta * sinPhi};
            // dP/du normalized; the closed form stays unit length at the poles where
            // the derivative itself vanishes.
            const math::float3 tangent{-sinPhi, 0.0f, cosPhi};
            mesh.vertices.push_back(makeVertex(normal * radius, normal, tangent, u, v));
        }
    }

    const std::uint32_t stride = segments + 1;
    mesh.indices.reserve(static_cast<std::size_t>(segments) * rings * 6);
    for (std::uint32_t ring = 0; ring < rings; ++ring) {
        for (std::uint32_t segment = 0; segment < segments; ++segment) {
            const std::uint32_t topLeft = ring * stride + segment;
            const std::uint32_t topRight = topLeft + 1;
            const std::uint32_t bottomLeft = topLeft + stride;
            const std::uint32_t bottomRight = bottomLeft + 1;
            // The pole rows collapse one quad edge; emitting its triangle would
            // leave a zero-area primitive behind.
            if (ring > 0) {
                mesh.indices.push_back(topLeft);
                mesh.indices.push_back(topRight);
                mesh.indices.push_back(bottomLeft);
            }
            if (ring + 1 < rings) {
                mesh.indices.push_back(topRight);
                mesh.indices.push_back(bottomRight);
                mesh.indices.push_back(bottomLeft);
            }
        }
    }

    mesh.localAabb = {{-radius, -radius, -radius}, {radius, radius, radius}};
    return mesh;
}

MeshData makeCube(float halfExtent) {
    halfExtent = std::max(halfExtent, kMinExtent);

    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    const math::float3 axes[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                  {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
    const math::float3 tangents[6] = {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f},
                                      {1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
    for (int face = 0; face < 6; ++face) {
        pushQuad(mesh, axes[face] * halfExtent, axes[face], tangents[face], halfExtent);
    }

    mesh.localAabb = {{-halfExtent, -halfExtent, -halfExtent},
                      {halfExtent, halfExtent, halfExtent}};
    return mesh;
}

MeshData makePlane(float halfExtent, std::uint32_t subdivisions) {
    halfExtent = std::max(halfExtent, kMinExtent);
    subdivisions = std::clamp(subdivisions, 1u, 64u);

    MeshData mesh;
    const std::uint32_t stride = subdivisions + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(stride) * stride);
    const math::float3 normal{0.0f, 1.0f, 0.0f};
    const math::float3 tangent{1.0f, 0.0f, 0.0f};
    for (std::uint32_t row = 0; row < stride; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(subdivisions);
        for (std::uint32_t column = 0; column < stride; ++column) {
            const float u = static_cast<float>(column) / static_cast<float>(subdivisions);
            const math::float3 position{(u * 2.0f - 1.0f) * halfExtent, 0.0f,
                                        (v * 2.0f - 1.0f) * halfExtent};
            mesh.vertices.push_back(makeVertex(position, normal, tangent, u, v));
        }
    }

    mesh.indices.reserve(static_cast<std::size_t>(subdivisions) * subdivisions * 6);
    for (std::uint32_t row = 0; row < subdivisions; ++row) {
        for (std::uint32_t column = 0; column < subdivisions; ++column) {
            const std::uint32_t nearLeft = row * stride + column;
            const std::uint32_t nearRight = nearLeft + 1;
            const std::uint32_t farLeft = nearLeft + stride;
            const std::uint32_t farRight = farLeft + 1;
            mesh.indices.push_back(nearLeft);
            mesh.indices.push_back(farLeft);
            mesh.indices.push_back(farRight);
            mesh.indices.push_back(nearLeft);
            mesh.indices.push_back(farRight);
            mesh.indices.push_back(nearRight);
        }
    }

    mesh.localAabb = {{-halfExtent, 0.0f, -halfExtent}, {halfExtent, 0.0f, halfExtent}};
    return mesh;
}

MeshData makeCylinder(float radius, float halfHeight, std::uint32_t segments) {
    radius = std::max(radius, kMinExtent);
    halfHeight = std::max(halfHeight, kMinExtent);
    segments = std::clamp(segments, kMinSegments, kMaxSegments);

    MeshData mesh;
    const std::uint32_t stride = segments + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(stride) * 2 + 2 * (stride + 1));
    mesh.indices.reserve(static_cast<std::size_t>(segments) * 12);

    // Side wall: top ring first (row 0) then bottom (row 1), matching
    // makeSphere's pole-to-equator row order; the outward normal is purely
    // radial since the wall doesn't slant.
    for (std::uint32_t row = 0; row < 2; ++row) {
        const float y = row == 0 ? halfHeight : -halfHeight;
        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            const math::float3 normal{cosPhi, 0.0f, sinPhi};
            const math::float3 tangent{-sinPhi, 0.0f, cosPhi};
            mesh.vertices.push_back(makeVertex({radius * cosPhi, y, radius * sinPhi}, normal,
                                               tangent, u, static_cast<float>(row)));
        }
    }
    pushRingBand(mesh, 0, stride, segments, false, false);

    pushDisk(mesh, halfHeight, radius, {0.0f, 1.0f, 0.0f}, false, segments);
    pushDisk(mesh, -halfHeight, radius, {0.0f, -1.0f, 0.0f}, true, segments);

    mesh.localAabb = {{-radius, -halfHeight, -radius}, {radius, halfHeight, radius}};
    return mesh;
}

MeshData makeCone(float radius, float halfHeight, std::uint32_t segments) {
    radius = std::max(radius, kMinExtent);
    halfHeight = std::max(halfHeight, kMinExtent);
    segments = std::clamp(segments, kMinSegments, kMaxSegments);

    MeshData mesh;
    const std::uint32_t stride = segments + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(stride) * 2 + (stride + 1));
    mesh.indices.reserve(static_cast<std::size_t>(segments) * 6);

    const float height = 2.0f * halfHeight;
    // Row 0 duplicates the apex once per column: the mantle is a ruled
    // surface, so its normal is constant along each generator but varies with
    // phi, and a single shared apex vertex could not carry that.
    for (std::uint32_t row = 0; row < 2; ++row) {
        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            const math::float3 normal =
                math::normalize(math::float3{height * cosPhi, radius, height * sinPhi});
            const math::float3 tangent{-sinPhi, 0.0f, cosPhi};
            const math::float3 position =
                row == 0 ? math::float3{0.0f, halfHeight, 0.0f}
                         : math::float3{radius * cosPhi, -halfHeight, radius * sinPhi};
            mesh.vertices.push_back(
                makeVertex(position, normal, tangent, u, static_cast<float>(row)));
        }
    }
    pushRingBand(mesh, 0, stride, segments, /*skipFirst=*/true, /*skipSecond=*/false);

    pushDisk(mesh, -halfHeight, radius, {0.0f, -1.0f, 0.0f}, true, segments);

    mesh.localAabb = {{-radius, -halfHeight, -radius}, {radius, halfHeight, radius}};
    return mesh;
}

MeshData makeTorus(float majorRadius, float minorRadius, std::uint32_t segments,
                   std::uint32_t rings) {
    majorRadius = std::max(majorRadius, kMinExtent * 2.0f);
    minorRadius = std::clamp(minorRadius, kMinExtent, majorRadius - kMinExtent);
    segments = std::clamp(segments, kMinSegments, kMaxSegments);
    rings = std::clamp(rings, 3u, 128u);

    MeshData mesh;
    const std::uint32_t stride = rings + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(segments + 1) * stride);
    // `major` sweeps once around the Y axis (the big loop), `minor` sweeps
    // once around the tube's cross-section; both are fully periodic, so
    // (unlike makeSphere) neither row collapses to a pole and every band is
    // a regular quad strip.
    for (std::uint32_t major = 0; major <= segments; ++major) {
        const float u = static_cast<float>(major) / static_cast<float>(segments);
        const float theta = u * 2.0f * kPi;
        const float cosTheta = std::cos(theta);
        const float sinTheta = std::sin(theta);
        const math::float3 tangent{-sinTheta, 0.0f, cosTheta};
        for (std::uint32_t minor = 0; minor <= rings; ++minor) {
            const float v = static_cast<float>(minor) / static_cast<float>(rings);
            const float phi = v * 2.0f * kPi;
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            const float tubeRadius = majorRadius + minorRadius * cosPhi;
            const math::float3 position{tubeRadius * cosTheta, minorRadius * sinPhi,
                                        tubeRadius * sinTheta};
            const math::float3 normal{cosPhi * cosTheta, sinPhi, cosPhi * sinTheta};
            mesh.vertices.push_back(makeVertex(position, normal, tangent, u, v));
        }
    }

    mesh.indices.reserve(static_cast<std::size_t>(segments) * rings * 6);
    for (std::uint32_t major = 0; major < segments; ++major) {
        pushRingBand(mesh, major * stride, (major + 1) * stride, rings, false, false);
    }

    const float outer = majorRadius + minorRadius;
    mesh.localAabb = {{-outer, -minorRadius, -outer}, {outer, minorRadius, outer}};
    return mesh;
}

MeshData makeCapsule(float radius, float halfHeight, std::uint32_t segments, std::uint32_t rings) {
    radius = std::max(radius, kMinExtent);
    halfHeight = std::max(halfHeight, kMinExtent);
    segments = std::clamp(segments, kMinSegments, kMaxSegments);
    rings = std::clamp(rings, 1u, 128u);

    MeshData mesh;
    const std::uint32_t stride = segments + 1;
    const std::uint32_t totalRows = 2 * rings + 2;
    mesh.vertices.reserve(static_cast<std::size_t>(stride) * totalRows);

    // Row k in [0, rings] sweeps the top hemisphere pole-to-equator; row k in
    // (rings, 2*rings+1] sweeps the bottom hemisphere equator-to-pole. The two
    // equator rows (k == rings and k == rings + 1) double as the cylindrical
    // body's rim, so the straight body needs no vertices of its own.
    for (std::uint32_t row = 0; row < totalRows; ++row) {
        const bool top = row <= rings;
        const float v = top ? static_cast<float>(row) / static_cast<float>(rings)
                            : static_cast<float>(row - rings - 1) / static_cast<float>(rings);
        const float theta = top ? v * (kPi * 0.5f) : kPi * 0.5f + v * (kPi * 0.5f);
        const float centerY = top ? halfHeight : -halfHeight;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        const float vCoord = static_cast<float>(row) / static_cast<float>(totalRows - 1);
        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            const math::float3 normal{sinTheta * cosPhi, cosTheta, sinTheta * sinPhi};
            const math::float3 tangent{-sinPhi, 0.0f, cosPhi};
            const math::float3 position{radius * sinTheta * cosPhi, centerY + radius * cosTheta,
                                        radius * sinTheta * sinPhi};
            mesh.vertices.push_back(makeVertex(position, normal, tangent, u, vCoord));
        }
    }

    mesh.indices.reserve(static_cast<std::size_t>(segments) * rings * 12);
    for (std::uint32_t row = 0; row + 1 < totalRows; ++row) {
        const bool skipFirst = row == 0;
        const bool skipSecond = row + 2 == totalRows;
        pushRingBand(mesh, row * stride, (row + 1) * stride, segments, skipFirst, skipSecond);
    }

    mesh.localAabb = {{-radius, -(halfHeight + radius), -radius},
                      {radius, halfHeight + radius, radius}};
    return mesh;
}

std::optional<MeshData> makePrimitive(std::string_view name, float size) {
    if (name == "sphere") {
        return makeSphere(size * 0.5f);
    }
    if (name == "cube") {
        return makeCube(size * 0.5f);
    }
    if (name == "plane") {
        return makePlane(size * 0.5f);
    }
    if (name == "cylinder") {
        return makeCylinder(size * 0.25f, size * 0.5f);
    }
    if (name == "cone") {
        return makeCone(size * 0.25f, size * 0.5f);
    }
    if (name == "torus") {
        return makeTorus(size * 0.375f, size * 0.125f);
    }
    if (name == "capsule") {
        return makeCapsule(size * 0.25f, size * 0.25f);
    }
    return std::nullopt;
}

} // namespace kumo::asset
