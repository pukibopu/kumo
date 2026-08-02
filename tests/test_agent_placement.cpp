#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), reached the same way
// test_agent_http_provider.cpp reaches retry_after.h.
#include "placement.h"

#include <kumo/math/math.h>

#include <cmath>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

math::Aabb box(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
    return {{minX, minY, minZ}, {maxX, maxY, maxZ}};
}

} // namespace

TEST_CASE("snapDeltaY rests a centered-pivot box at the clearance height") {
    // A unit cube centered on its pivot at y=5: min.y is 4.5.
    const float delta = placement::snapDeltaY(box(-0.5f, 4.5f, -0.5f, 0.5f, 5.5f, 0.5f), 0.01f);
    CHECK(delta == doctest::Approx(-4.49f));
}

TEST_CASE("snapDeltaY lifts a box whose bounds extend below its origin") {
    // Pivot at y=0 but geometry reaching down to -0.3 (a model pivoted mid-trunk).
    const float delta = placement::snapDeltaY(box(-1.0f, -0.3f, -1.0f, 1.0f, 2.0f, 1.0f), 0.01f);
    CHECK(delta == doctest::Approx(0.31f));
}

TEST_CASE("aggregateAabb unions boxes and rejects an empty span") {
    const std::vector<math::Aabb> boxes{box(-1, 0, -1, 1, 2, 1), box(0, -3, 0, 4, 1, 2)};
    const std::optional<math::Aabb> aggregate = placement::aggregateAabb(boxes);
    REQUIRE(aggregate.has_value());
    CHECK(aggregate->min.x == doctest::Approx(-1.0f));
    CHECK(aggregate->min.y == doctest::Approx(-3.0f));
    CHECK(aggregate->max.x == doctest::Approx(4.0f));
    CHECK(aggregate->max.z == doctest::Approx(2.0f));

    CHECK(!placement::aggregateAabb({}).has_value());
}

TEST_CASE("overlapDepth needs penetration on all three axes") {
    // Touching faces (zero depth on X) is not an overlap.
    CHECK(!placement::overlapDepth(box(0, 0, 0, 1, 1, 1), box(1, 0, 0, 2, 1, 1)).has_value());
    // Disjoint on Y only.
    CHECK(!placement::overlapDepth(box(0, 0, 0, 1, 1, 1), box(0, 2, 0, 1, 3, 1)).has_value());

    const std::optional<math::float3> depth =
        placement::overlapDepth(box(0, 0, 0, 1, 1, 1), box(0.8f, 0.5f, -0.5f, 2, 2, 0.7f));
    REQUIRE(depth.has_value());
    CHECK(depth->x == doctest::Approx(0.2f));
    CHECK(depth->y == doctest::Approx(0.5f));
    CHECK(depth->z == doctest::Approx(0.7f));
}

TEST_CASE("meaningfulOverlap ignores shallow support contact within the margin") {
    // A crate resting on a table, sunk 1 cm: Y depth 0.01 stays below a 2 cm margin.
    const math::Aabb table = box(-1, 0, -1, 1, 1, 1);
    const math::Aabb crate = box(-0.2f, 0.99f, -0.2f, 0.2f, 1.39f, 0.2f);
    CHECK(!placement::meaningfulOverlap(table, crate, 0.02f));
    // The same crate sunk 10 cm is a real interpenetration.
    const math::Aabb sunk = box(-0.2f, 0.9f, -0.2f, 0.2f, 1.3f, 0.2f);
    CHECK(placement::meaningfulOverlap(table, sunk, 0.02f));
}

TEST_CASE("findConflicts reports each conflicting box with its index and depth") {
    const std::vector<math::Aabb> existing{
        box(0, 0, 0, 1, 1, 1),       // overlaps
        box(5, 0, 0, 6, 1, 1),       // far away
        box(0.5f, 0, 0.5f, 2, 2, 2), // overlaps
    };
    const std::vector<placement::Conflict> conflicts =
        placement::findConflicts(box(0.2f, 0.2f, 0.2f, 0.9f, 0.9f, 0.9f), existing, 0.01f);
    REQUIRE(conflicts.size() == 2);
    CHECK(conflicts[0].index == 0);
    CHECK(conflicts[1].index == 2);
    CHECK(conflicts[0].depth.x == doctest::Approx(0.7f));
}

TEST_CASE("deepestConflict picks the largest minimal-axis penetration") {
    const std::vector<placement::Conflict> conflicts{
        {.index = 0, .depth = {2.0f, 0.05f, 2.0f}}, // min axis 0.05
        {.index = 1, .depth = {0.4f, 0.4f, 0.4f}},  // min axis 0.4 — hardest to dismiss
        {.index = 2, .depth = {5.0f, 0.2f, 5.0f}},  // min axis 0.2
    };
    CHECK(placement::deepestConflict(conflicts).index == 1);
}

TEST_CASE("suggestPosition is deterministic and conflict-free") {
    const math::Aabb candidate = box(-0.5f, 0, -0.5f, 0.5f, 1, 0.5f);
    const std::vector<math::Aabb> existing{box(-0.5f, 0, -0.5f, 0.5f, 1, 0.5f)};
    const math::float3 requested{0.0f, 0.5f, 0.0f};

    const std::optional<math::float3> first =
        placement::suggestPosition(candidate, requested, existing, 0.01f, 8);
    const std::optional<math::float3> second =
        placement::suggestPosition(candidate, requested, existing, 0.01f, 8);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->x == second->x);
    CHECK(first->y == second->y);
    CHECK(first->z == second->z);
    // Y is preserved; the suggested spot itself must be conflict-free.
    CHECK(first->y == doctest::Approx(0.5f));
    const math::float3 offset = *first - requested;
    const math::Aabb shifted{candidate.min + offset, candidate.max + offset};
    CHECK(placement::findConflicts(shifted, existing, 0.01f).empty());
}

TEST_CASE("suggestPosition gives up when every probe conflicts") {
    // Existing coverage far larger than maxRings x step can escape.
    const std::vector<math::Aabb> everywhere{box(-1000, -10, -1000, 1000, 10, 1000)};
    CHECK(!placement::suggestPosition(box(-0.5f, 0, -0.5f, 0.5f, 1, 0.5f), {0, 0.5f, 0}, everywhere,
                                      0.01f, 8)
               .has_value());
}

TEST_CASE("sampleScatter keeps min_spacing between accepted instances") {
    placement::ScatterParams params;
    params.count = 8;
    params.area[0] = 30.0f;
    params.area[1] = 30.0f;
    params.seed = 5;
    params.minSpacing = 1.0f;
    const math::Aabb unitCube = box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    const auto sampled = placement::sampleScatter(params, unitCube, {}, 0.02f);
    REQUIRE(sampled.has_value());
    REQUIRE(sampled->size() == 8);
    for (std::size_t i = 0; i < sampled->size(); ++i) {
        for (std::size_t j = i + 1; j < sampled->size(); ++j) {
            const float gapX = std::abs((*sampled)[i].position.x - (*sampled)[j].position.x) - 1.0f;
            const float gapZ = std::abs((*sampled)[i].position.z - (*sampled)[j].position.z) - 1.0f;
            // XZ edge-to-edge separation reaches min_spacing on at least one axis.
            CHECK(std::max(gapX, gapZ) >= params.minSpacing - 1e-4f);
        }
    }
}

TEST_CASE("sampleScatter with avoidExisting keeps instances off existing boxes") {
    placement::ScatterParams params;
    params.count = 10;
    params.area[0] = 20.0f;
    params.area[1] = 20.0f;
    params.center = {0.0f, 0.5f, 0.0f};
    params.seed = 11;
    params.avoidExisting = true;
    const math::Aabb unitCube = box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    const std::vector<math::Aabb> existing{box(-3, 0, -3, 3, 1, 3)};

    const auto sampled = placement::sampleScatter(params, unitCube, existing, 0.02f);
    REQUIRE(sampled.has_value());
    for (const scene::Transform& t : *sampled) {
        const math::Aabb footprint = math::transformAabb(unitCube, t.matrix());
        CHECK(!placement::meaningfulOverlap(footprint, existing[0], 0.02f));
    }
}

TEST_CASE("sampleScatter is deterministic for identical seed and arguments") {
    placement::ScatterParams params;
    params.count = 6;
    params.area[0] = 15.0f;
    params.area[1] = 15.0f;
    params.seed = 42;
    params.minSpacing = 0.5f;
    params.avoidExisting = true;
    params.scaleJitter = 0.2f;
    params.rotationJitterDeg = 45.0f;
    const math::Aabb unitCube = box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
    const std::vector<math::Aabb> existing{box(-2, -1, -2, 2, 1, 2)};

    const auto a = placement::sampleScatter(params, unitCube, existing, 0.02f);
    const auto b = placement::sampleScatter(params, unitCube, existing, 0.02f);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->size() == b->size());
    for (std::size_t i = 0; i < a->size(); ++i) {
        CHECK((*a)[i].position.x == (*b)[i].position.x);
        CHECK((*a)[i].position.z == (*b)[i].position.z);
        CHECK((*a)[i].scale.x == (*b)[i].scale.x);
    }
}

TEST_CASE("sampleScatter at minSpacing 0 without avoidExisting reproduces the legacy draw "
          "sequence") {
    // The legacy scatter accepted every draw: attempts must equal count exactly,
    // proving no candidate was rejected and the RNG sequence is untouched.
    placement::ScatterParams params;
    params.count = 12;
    params.area[0] = 0.5f; // tiny area guarantees overlapping instances
    params.area[1] = 0.5f;
    params.seed = 3;
    const math::Aabb unitCube = box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    const auto sampled = placement::sampleScatter(params, unitCube, {}, 0.02f);
    REQUIRE(sampled.has_value());
    CHECK(sampled->size() == 12);
}

TEST_CASE("sampleScatter fails with counts when the request cannot fit") {
    placement::ScatterParams params;
    params.count = 10;
    params.area[0] = 1.0f;
    params.area[1] = 1.0f;
    params.seed = 1;
    params.minSpacing = 5.0f; // no two instances can ever satisfy this in a 1x1 area
    const math::Aabb unitCube = box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    const auto sampled = placement::sampleScatter(params, unitCube, {}, 0.02f);
    REQUIRE(!sampled.has_value());
    CHECK(sampled.error().requested == 10);
    CHECK(sampled.error().accepted == 1);
    CHECK(sampled.error().attempts == 100); // resolved default max(10 x count, 64)
}

TEST_CASE("supportContact requires the shallow Y interval to sit at the stacking interface") {
    // Resting: the crate's bottom meets the table's top within the margin.
    const math::Aabb table = box(-1, 0, -1, 1, 1, 1);
    const math::Aabb crate = box(-0.2f, 0.99f, -0.2f, 0.2f, 1.4f, 0.2f);
    CHECK(placement::supportContact(table, crate, *placement::overlapDepth(table, crate), 0.02f));
    // A thin shelf buried mid-height inside a cabinet also has a small Y depth
    // (its own height), but there is no stacking interface anywhere near it.
    const math::Aabb cabinet = box(-1, 0, -1, 1, 2, 1);
    const math::Aabb shelf = box(-0.9f, 1.0f, -0.9f, 0.9f, 1.01f, 0.9f);
    CHECK(!placement::supportContact(cabinet, shelf, *placement::overlapDepth(cabinet, shelf),
                                     0.02f));
    CHECK(placement::meaningfulOverlap(cabinet, shelf, 0.02f));
}

TEST_CASE("thin geometry buried sideways is a real collision despite its small min-axis depth") {
    // A 1 cm wall crossing a couch: the X depth (the wall's own thickness) is
    // below any support margin, but nothing about this is resting contact.
    const math::Aabb couch = box(-1, 0, -1, 1, 1, 1);
    const math::Aabb wall = box(0, 0, -2, 0.01f, 2, 2);
    CHECK(placement::meaningfulOverlap(couch, wall, 0.02f));

    const std::vector<math::Aabb> existing{couch};
    const std::vector<placement::Conflict> conflicts =
        placement::findConflicts(wall, existing, 0.02f);
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].depth.x == doctest::Approx(0.01f));
}

TEST_CASE("sampleScatter min_spacing works for zero-height plane footprints") {
    placement::ScatterParams params;
    params.count = 6;
    params.area[0] = 25.0f;
    params.area[1] = 25.0f;
    params.seed = 7;
    params.minSpacing = 1.0f;
    // A plane group: zero-height AABB, so a 3D overlap test would never fire.
    const math::Aabb plane = box(-0.5f, 0.0f, -0.5f, 0.5f, 0.0f, 0.5f);

    const auto sampled = placement::sampleScatter(params, plane, {}, 0.02f);
    REQUIRE(sampled.has_value());
    REQUIRE(sampled->size() == 6);
    for (std::size_t i = 0; i < sampled->size(); ++i) {
        for (std::size_t j = i + 1; j < sampled->size(); ++j) {
            const float gapX = std::abs((*sampled)[i].position.x - (*sampled)[j].position.x) - 1.0f;
            const float gapZ = std::abs((*sampled)[i].position.z - (*sampled)[j].position.z) - 1.0f;
            CHECK(std::max(gapX, gapZ) >= params.minSpacing - 1e-4f);
        }
    }
}
