#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <kumo/math/math.h>

#include <cmath>

using namespace kumo;

namespace {

float ndcDepth(const math::float4x4& proj, float viewZ) {
    math::float4 clip = proj * math::float4(0.0f, 0.0f, viewZ, 1.0f);
    return clip.z / clip.w;
}

} // namespace

TEST_CASE("perspective projection is reversed-Z") {
    math::float4x4 proj = math::perspective(math::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    CHECK(std::abs(ndcDepth(proj, -0.1f) - 1.0f) < 1e-5f);
    CHECK(std::abs(ndcDepth(proj, -100.0f)) < 1e-4f);
    CHECK(ndcDepth(proj, -1.0f) > ndcDepth(proj, -50.0f));
}

TEST_CASE("lookAt places the target in front of the camera along -Z") {
    math::float4x4 view = math::lookAt({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    math::float4 origin = view * math::float4(0.0f, 0.0f, 0.0f, 1.0f);

    CHECK(std::abs(origin.x) < 1e-6f);
    CHECK(std::abs(origin.y) < 1e-6f);
    CHECK(std::abs(origin.z + 5.0f) < 1e-6f);
}

TEST_CASE("basic vector operations") {
    math::float3 x{1.0f, 0.0f, 0.0f};
    math::float3 y{0.0f, 1.0f, 0.0f};

    math::float3 z = math::cross(x, y);
    CHECK(std::abs(z.z - 1.0f) < 1e-6f);
    CHECK(math::dot(x, y) == 0.0f);
}
