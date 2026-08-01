#include <doctest/doctest.h>

// Private kumo_renderer header (engine/renderer/src), not a public interface.
#include "shadow.h"

using namespace kumo;

namespace {

math::float3 ndcOf(const math::float4x4& m, const math::float3& worldPos) {
    const math::float4 clip = m * math::float4(worldPos, 1.0f);
    return math::float3(clip) / clip.w;
}

} // namespace

TEST_CASE("fitDirectionalShadow: every AABB corner lands inside the clip volume") {
    const math::Aabb bounds{.min = {-5.0f, -2.0f, -3.0f}, .max = {5.0f, 4.0f, 7.0f}};
    const math::float3 lightDir = math::normalize(math::float3{0.3f, -1.0f, 0.2f});
    const math::float4x4 m = renderer::detail::fitDirectionalShadow(bounds, lightDir);

    constexpr float kEps = 1e-3f;
    for (int i = 0; i < 8; ++i) {
        const math::float3 corner{(i & 1) != 0 ? bounds.max.x : bounds.min.x,
                                  (i & 2) != 0 ? bounds.max.y : bounds.min.y,
                                  (i & 4) != 0 ? bounds.max.z : bounds.min.z};
        const math::float3 ndc = ndcOf(m, corner);
        CAPTURE(i);
        CHECK(ndc.x >= -1.0f - kEps);
        CHECK(ndc.x <= 1.0f + kEps);
        CHECK(ndc.y >= -1.0f - kEps);
        CHECK(ndc.y <= 1.0f + kEps);
        CHECK(ndc.z >= 0.0f - kEps);
        CHECK(ndc.z <= 1.0f + kEps);
    }
}

TEST_CASE("fitDirectionalShadow: straight-down light does not hit the up-vector degeneracy") {
    const math::Aabb bounds{.min = {-1.0f, -1.0f, -1.0f}, .max = {1.0f, 1.0f, 1.0f}};
    const math::float3 lightDir{0.0f, -1.0f, 0.0f};
    const math::float4x4 m = renderer::detail::fitDirectionalShadow(bounds, lightDir);
    CHECK(m != math::float4x4(1.0f));

    for (int i = 0; i < 8; ++i) {
        const math::float3 corner{(i & 1) != 0 ? bounds.max.x : bounds.min.x,
                                  (i & 2) != 0 ? bounds.max.y : bounds.min.y,
                                  (i & 4) != 0 ? bounds.max.z : bounds.min.z};
        const math::float3 ndc = ndcOf(m, corner);
        CAPTURE(i);
        CHECK(ndc.x >= -1.0f - 1e-3f);
        CHECK(ndc.x <= 1.0f + 1e-3f);
        CHECK(ndc.y >= -1.0f - 1e-3f);
        CHECK(ndc.y <= 1.0f + 1e-3f);
    }
}

TEST_CASE("fitDirectionalShadow: degenerate (inverted) AABB returns identity") {
    const math::Aabb bounds{.min = {1.0f, 1.0f, 1.0f}, .max = {-1.0f, -1.0f, -1.0f}};
    const math::float4x4 m =
        renderer::detail::fitDirectionalShadow(bounds, math::float3{0.0f, -1.0f, 0.0f});
    CHECK(m == math::float4x4(1.0f));
}

TEST_CASE("fitDirectionalShadow: a single-point AABB (zero radius) returns identity") {
    const math::Aabb bounds{.min = {0.0f, 0.0f, 0.0f}, .max = {0.0f, 0.0f, 0.0f}};
    const math::float4x4 m =
        renderer::detail::fitDirectionalShadow(bounds, math::float3{0.0f, -1.0f, 0.0f});
    CHECK(m == math::float4x4(1.0f));
}
