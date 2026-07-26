#include <doctest/doctest.h>

#include <kumo/asset/primitives.h>
#include <kumo/math/math.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace kumo;

namespace {

math::float3 position(const asset::Vertex& v) {
    return {v.px, v.py, v.pz};
}

math::float3 normal(const asset::Vertex& v) {
    return {v.nx, v.ny, v.nz};
}

math::float3 tangent(const asset::Vertex& v) {
    return {v.tx, v.ty, v.tz};
}

bool indicesInRange(const asset::MeshData& mesh) {
    for (std::uint32_t index : mesh.indices) {
        if (static_cast<std::size_t>(index) >= mesh.vertices.size()) {
            return false;
        }
    }
    return true;
}

bool allFinite(const asset::MeshData& mesh) {
    for (const asset::Vertex& v : mesh.vertices) {
        const float components[] = {v.px, v.py, v.pz, v.nx, v.ny, v.nz,
                                    v.tx, v.ty, v.tz, v.tw, v.u,  v.v};
        for (float c : components) {
            if (!std::isfinite(c)) {
                return false;
            }
        }
    }
    return true;
}

bool unitNormalsAndTangents(const asset::MeshData& mesh) {
    for (const asset::Vertex& v : mesh.vertices) {
        if (std::abs(math::length(normal(v)) - 1.0f) > 1e-4f) {
            return false;
        }
        if (std::abs(math::length(tangent(v)) - 1.0f) > 1e-4f) {
            return false;
        }
        if (std::abs(math::dot(normal(v), tangent(v))) > 1e-4f) {
            return false;
        }
        if (v.tw != 1.0f) {
            return false;
        }
    }
    return true;
}

// Front faces wind counter-clockwise seen from outside, so the geometric normal
// must agree with the vertex normals it interpolates.
bool windsOutward(const asset::MeshData& mesh) {
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const asset::Vertex& a = mesh.vertices[mesh.indices[i]];
        const asset::Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const asset::Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const math::float3 face = math::cross(position(b) - position(a), position(c) - position(a));
        const math::float3 shading = normal(a) + normal(b) + normal(c);
        if (math::dot(face, shading) <= 0.0f) {
            return false;
        }
    }
    return true;
}

bool uvInUnitRange(const asset::MeshData& mesh) {
    for (const asset::Vertex& v : mesh.vertices) {
        if (v.u < -1e-5f || v.u > 1.0f + 1e-5f || v.v < -1e-5f || v.v > 1.0f + 1e-5f) {
            return false;
        }
    }
    return true;
}

void checkAabb(const math::Aabb& box, const math::float3& min, const math::float3& max) {
    CHECK(std::abs(box.min.x - min.x) < 1e-6f);
    CHECK(std::abs(box.min.y - min.y) < 1e-6f);
    CHECK(std::abs(box.min.z - min.z) < 1e-6f);
    CHECK(std::abs(box.max.x - max.x) < 1e-6f);
    CHECK(std::abs(box.max.y - max.y) < 1e-6f);
    CHECK(std::abs(box.max.z - max.z) < 1e-6f);
}

} // namespace

TEST_CASE("makeSphere has a lat-long topology with a duplicated seam column") {
    const std::uint32_t segments = 24;
    const std::uint32_t rings = 12;
    asset::MeshData mesh = asset::makeSphere(0.75f, segments, rings);

    CHECK(mesh.vertices.size() == static_cast<std::size_t>(segments + 1) * (rings + 1));
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(mesh.materialIndex == -1);

    // The seam column repeats the first column's position so u spans the full range.
    const std::size_t equator = static_cast<std::size_t>(rings / 2) * (segments + 1);
    const asset::Vertex& first = mesh.vertices[equator];
    const asset::Vertex& seam = mesh.vertices[equator + segments];
    CHECK(std::abs(first.u - 0.0f) < 1e-6f);
    CHECK(std::abs(seam.u - 1.0f) < 1e-6f);
    CHECK(math::length(position(first) - position(seam)) < 1e-5f);
    CHECK(std::abs(first.py) < 1e-6f);
}

TEST_CASE("makeSphere vertices lie on the sphere with an outward frame") {
    const float radius = 0.75f;
    asset::MeshData mesh = asset::makeSphere(radius, 24, 12);

    bool onSphere = true;
    bool normalMatchesPosition = true;
    for (const asset::Vertex& v : mesh.vertices) {
        if (std::abs(math::length(position(v)) - radius) > 1e-4f) {
            onSphere = false;
        }
        if (math::length(normal(v) - position(v) / radius) > 1e-4f) {
            normalMatchesPosition = false;
        }
    }
    CHECK(onSphere);
    CHECK(normalMatchesPosition);
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
}

TEST_CASE("makeSphere bounds the analytic sphere") {
    asset::MeshData mesh = asset::makeSphere(2.0f, 16, 8);
    checkAabb(mesh.localAabb, {-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f});
}

TEST_CASE("makeCube has 24 vertices and 6 independent faces") {
    const float halfExtent = 0.5f;
    asset::MeshData mesh = asset::makeCube(halfExtent);

    CHECK(mesh.vertices.size() == 24);
    CHECK(mesh.indices.size() == 36);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    bool onCorners = true;
    for (const asset::Vertex& v : mesh.vertices) {
        const float coords[] = {v.px, v.py, v.pz};
        for (float c : coords) {
            if (std::abs(std::abs(c) - halfExtent) > 1e-6f) {
                onCorners = false;
            }
        }
    }
    CHECK(onCorners);

    std::vector<math::float3> distinct;
    for (const asset::Vertex& v : mesh.vertices) {
        bool seen = false;
        for (const math::float3& n : distinct) {
            if (math::length(n - normal(v)) < 1e-5f) {
                seen = true;
            }
        }
        if (!seen) {
            distinct.push_back(normal(v));
        }
    }
    CHECK(distinct.size() == 6);

    checkAabb(mesh.localAabb, {-halfExtent, -halfExtent, -halfExtent},
              {halfExtent, halfExtent, halfExtent});
}

TEST_CASE("makePlane is a subdivided XZ grid facing +Y") {
    const float halfExtent = 1.5f;
    const std::uint32_t subdivisions = 4;
    asset::MeshData mesh = asset::makePlane(halfExtent, subdivisions);

    CHECK(mesh.vertices.size() == static_cast<std::size_t>(subdivisions + 1) * (subdivisions + 1));
    CHECK(mesh.indices.size() == static_cast<std::size_t>(subdivisions) * subdivisions * 6);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    bool flat = true;
    bool facesUp = true;
    for (const asset::Vertex& v : mesh.vertices) {
        if (v.py != 0.0f) {
            flat = false;
        }
        if (math::length(normal(v) - math::float3(0.0f, 1.0f, 0.0f)) > 1e-6f) {
            facesUp = false;
        }
    }
    CHECK(flat);
    CHECK(facesUp);

    checkAabb(mesh.localAabb, {-halfExtent, 0.0f, -halfExtent}, {halfExtent, 0.0f, halfExtent});
}

TEST_CASE("primitive builders clamp degenerate arguments") {
    asset::MeshData sphere = asset::makeSphere(0.0f, 0, 0);
    CHECK(sphere.vertices.size() == 4 * 3);
    CHECK(sphere.indices.size() >= 3);
    CHECK(indicesInRange(sphere));
    CHECK(allFinite(sphere));
    CHECK(unitNormalsAndTangents(sphere));
    CHECK(sphere.localAabb.max.x > sphere.localAabb.min.x);

    asset::MeshData cube = asset::makeCube(-1.0f);
    CHECK(cube.vertices.size() == 24);
    CHECK(allFinite(cube));
    CHECK(cube.localAabb.max.y > cube.localAabb.min.y);

    asset::MeshData plane = asset::makePlane(0.0f, 0);
    CHECK(plane.vertices.size() == 4);
    CHECK(plane.indices.size() == 6);
    CHECK(indicesInRange(plane));
    CHECK(allFinite(plane));
    CHECK(plane.localAabb.max.x > plane.localAabb.min.x);

    asset::MeshData dense = asset::makeSphere(1.0f, 4096, 4096);
    CHECK(dense.vertices.size() == static_cast<std::size_t>(257) * 129);
    CHECK(indicesInRange(dense));
}
