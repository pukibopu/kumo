#include <doctest/doctest.h>

#include <kumo/asset/primitives.h>
#include <kumo/math/math.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
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

TEST_CASE("makeCylinder is a capped tube facing outward and up/down") {
    const float radius = 0.6f;
    const float halfHeight = 0.9f;
    const std::uint32_t segments = 20;
    asset::MeshData mesh = asset::makeCylinder(radius, halfHeight, segments);

    // Side (2 rows) + two disk caps (1 center + segments+1 rim each).
    CHECK(mesh.vertices.size() == static_cast<std::size_t>(4 * segments + 6));
    CHECK(mesh.indices.size() == static_cast<std::size_t>(12 * segments));
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    bool sideOnCylinder = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(2) * (segments + 1); ++i) {
        const asset::Vertex& v = mesh.vertices[i];
        if (std::abs(std::sqrt(v.px * v.px + v.pz * v.pz) - radius) > 1e-4f) {
            sideOnCylinder = false;
        }
        if (std::abs(std::abs(v.py) - halfHeight) > 1e-4f) {
            sideOnCylinder = false;
        }
    }
    CHECK(sideOnCylinder);

    checkAabb(mesh.localAabb, {-radius, -halfHeight, -radius}, {radius, halfHeight, radius});
}

TEST_CASE("makeCone is a capped mantle with per-generator normals") {
    const float radius = 0.4f;
    const float halfHeight = 0.7f;
    const std::uint32_t segments = 16;
    asset::MeshData mesh = asset::makeCone(radius, halfHeight, segments);

    // Side (apex row + rim row) + one bottom disk cap.
    CHECK(mesh.vertices.size() == static_cast<std::size_t>(3 * segments + 4));
    CHECK(mesh.indices.size() == static_cast<std::size_t>(6 * segments));
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    // Every apex-row vertex sits at the same point even though its normal
    // differs by column (the mantle's normal varies with phi at the apex).
    bool apexAtPeak = true;
    for (std::size_t i = 0; i <= segments; ++i) {
        const asset::Vertex& v = mesh.vertices[i];
        if (std::abs(v.px) > 1e-5f || std::abs(v.py - halfHeight) > 1e-5f ||
            std::abs(v.pz) > 1e-5f) {
            apexAtPeak = false;
        }
    }
    CHECK(apexAtPeak);

    checkAabb(mesh.localAabb, {-radius, -halfHeight, -radius}, {radius, halfHeight, radius});
}

TEST_CASE("makeTorus is a fully periodic tube with no poles") {
    const float majorRadius = 0.5f;
    const float minorRadius = 0.2f;
    const std::uint32_t segments = 24;
    const std::uint32_t rings = 12;
    asset::MeshData mesh = asset::makeTorus(majorRadius, minorRadius, segments, rings);

    CHECK(mesh.vertices.size() == static_cast<std::size_t>(segments + 1) * (rings + 1));
    CHECK(mesh.indices.size() == static_cast<std::size_t>(segments) * rings * 6);
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    bool onTorus = true;
    for (const asset::Vertex& v : mesh.vertices) {
        const float radial = std::sqrt(v.px * v.px + v.pz * v.pz);
        const float tubeDistance =
            std::sqrt((radial - majorRadius) * (radial - majorRadius) + v.py * v.py);
        if (std::abs(tubeDistance - minorRadius) > 1e-3f) {
            onTorus = false;
        }
    }
    CHECK(onTorus);

    const float outer = majorRadius + minorRadius;
    checkAabb(mesh.localAabb, {-outer, -minorRadius, -outer}, {outer, minorRadius, outer});
}

TEST_CASE("makeTorus clamps the minor radius below the major radius") {
    asset::MeshData mesh = asset::makeTorus(0.2f, 5.0f, 12, 6);
    CHECK(allFinite(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    // A self-intersecting tube would push the AABB well past 2x majorRadius.
    CHECK(mesh.localAabb.max.x < 0.4f);
}

TEST_CASE("makeCapsule is a cylinder body with hemisphere caps sharing the rim rings") {
    const float radius = 0.3f;
    const float halfHeight = 0.5f;
    const std::uint32_t segments = 16;
    const std::uint32_t rings = 6;
    asset::MeshData mesh = asset::makeCapsule(radius, halfHeight, segments, rings);

    CHECK(mesh.vertices.size() == static_cast<std::size_t>(segments + 1) * (2 * rings + 2));
    CHECK(mesh.indices.size() == static_cast<std::size_t>(12) * segments * rings);
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(indicesInRange(mesh));
    CHECK(allFinite(mesh));
    CHECK(uvInUnitRange(mesh));
    CHECK(unitNormalsAndTangents(mesh));
    CHECK(windsOutward(mesh));
    CHECK(mesh.materialIndex == -1);

    bool onCapsule = true;
    for (const asset::Vertex& v : mesh.vertices) {
        const float clampedY = std::clamp(v.py, -halfHeight, halfHeight);
        const float dx = v.px;
        const float dy = v.py - clampedY;
        const float dz = v.pz;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::abs(distance - radius) > 1e-4f) {
            onCapsule = false;
        }
    }
    CHECK(onCapsule);

    checkAabb(mesh.localAabb, {-radius, -(halfHeight + radius), -radius},
              {radius, halfHeight + radius, radius});
}

TEST_CASE("primitive builders (4-7) clamp degenerate arguments") {
    asset::MeshData cylinder = asset::makeCylinder(0.0f, 0.0f, 0);
    CHECK(allFinite(cylinder));
    CHECK(indicesInRange(cylinder));
    CHECK(unitNormalsAndTangents(cylinder));
    CHECK(cylinder.localAabb.max.x > cylinder.localAabb.min.x);

    asset::MeshData cone = asset::makeCone(-1.0f, -1.0f, 1);
    CHECK(allFinite(cone));
    CHECK(indicesInRange(cone));
    CHECK(unitNormalsAndTangents(cone));
    CHECK(cone.localAabb.max.y > cone.localAabb.min.y);

    asset::MeshData torus = asset::makeTorus(0.0f, 0.0f, 0, 0);
    CHECK(allFinite(torus));
    CHECK(indicesInRange(torus));
    CHECK(unitNormalsAndTangents(torus));
    CHECK(torus.localAabb.max.x > torus.localAabb.min.x);

    asset::MeshData capsule = asset::makeCapsule(0.0f, 0.0f, 0, 0);
    CHECK(allFinite(capsule));
    CHECK(indicesInRange(capsule));
    CHECK(unitNormalsAndTangents(capsule));
    CHECK(capsule.localAabb.max.y > capsule.localAabb.min.y);
}

TEST_CASE("makePrimitive maps all seven names and rejects unknown ones") {
    const std::string_view names[] = {"sphere", "cube",  "plane",  "cylinder",
                                      "cone",   "torus", "capsule"};
    for (std::string_view name : names) {
        const std::optional<asset::MeshData> mesh = asset::makePrimitive(name, 2.0f);
        REQUIRE_MESSAGE(mesh.has_value(), name);
        CHECK(!mesh->vertices.empty());
        CHECK(!mesh->indices.empty());
    }
    CHECK(!asset::makePrimitive("torus_knot", 1.0f).has_value());
    CHECK(!asset::makePrimitive("", 1.0f).has_value());
}

TEST_CASE("makePrimitive's size parameter flows into the AABB extent") {
    const float size = 2.4f;
    // sphere/cube/plane: overall extent equals size on every non-flat axis.
    CHECK(asset::makePrimitive("sphere", size)->localAabb.max.x * 2.0f ==
          doctest::Approx(size).epsilon(0.01));
    CHECK(asset::makePrimitive("cube", size)->localAabb.max.x * 2.0f ==
          doctest::Approx(size).epsilon(0.01));
    CHECK(asset::makePrimitive("plane", size)->localAabb.max.x * 2.0f ==
          doctest::Approx(size).epsilon(0.01));
    // cylinder/cone/capsule: `size` is the overall height.
    const asset::MeshData cylinder = *asset::makePrimitive("cylinder", size);
    CHECK(cylinder.localAabb.max.y - cylinder.localAabb.min.y == doctest::Approx(size));
    const asset::MeshData cone = *asset::makePrimitive("cone", size);
    CHECK(cone.localAabb.max.y - cone.localAabb.min.y == doctest::Approx(size));
    const asset::MeshData capsule = *asset::makePrimitive("capsule", size);
    CHECK(capsule.localAabb.max.y - capsule.localAabb.min.y == doctest::Approx(size));
    // torus: `size` is the overall footprint diameter (major loop + tube).
    const asset::MeshData torus = *asset::makePrimitive("torus", size);
    CHECK(torus.localAabb.max.x - torus.localAabb.min.x == doctest::Approx(size));
}
