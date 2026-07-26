#include <kumo/asset/primitives.h>

#include <algorithm>
#include <cmath>

namespace kumo::asset {
namespace {

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

} // namespace kumo::asset
