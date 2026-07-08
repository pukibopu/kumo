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

TEST_CASE("perspectiveInfinite is reversed-Z with far at infinity") {
    const float nearZ = 0.1f;
    math::float4x4 proj = math::perspectiveInfinite(math::radians(60.0f), 16.0f / 9.0f, nearZ);

    CHECK(std::abs(ndcDepth(proj, -nearZ) - 1.0f) < 1e-5f);
    CHECK(ndcDepth(proj, -1.0e6f * nearZ) < 1e-4f);
    CHECK(ndcDepth(proj, -1.0f) > ndcDepth(proj, -100.0f));
    CHECK(ndcDepth(proj, -100.0f) > ndcDepth(proj, -10000.0f));
}

TEST_CASE("quatLookAt orients -Z onto the target direction") {
    math::float3 eye{1.0f, 2.0f, 3.0f};
    math::float3 target{-4.0f, 0.5f, 2.0f};
    math::float3 dir = math::normalize(target - eye);

    math::quat q = math::quatLookAt(target - eye, {0.0f, 1.0f, 0.0f});
    math::float3 fwd = q * math::float3(0.0f, 0.0f, -1.0f);

    CHECK(std::abs(fwd.x - dir.x) < 1e-5f);
    CHECK(std::abs(fwd.y - dir.y) < 1e-5f);
    CHECK(std::abs(fwd.z - dir.z) < 1e-5f);
}

TEST_CASE("quatLookAt stays finite when up is parallel to forward") {
    math::quat q = math::quatLookAt({0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    math::float3 fwd = q * math::float3(0.0f, 0.0f, -1.0f);

    CHECK(std::isfinite(q.x));
    CHECK(std::isfinite(q.y));
    CHECK(std::isfinite(q.z));
    CHECK(std::isfinite(q.w));
    CHECK(std::abs(fwd.x) < 1e-5f);
    CHECK(std::abs(fwd.y + 1.0f) < 1e-5f);
    CHECK(std::abs(fwd.z) < 1e-5f);
}

TEST_CASE("quatLookAt degrades gracefully for a zero-length forward") {
    math::quat q = math::quatLookAt({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    CHECK(std::isfinite(q.w));
}

TEST_CASE("trs composes as T*R*S") {
    math::float3 t{1.0f, 2.0f, 3.0f};
    math::quat r = math::quatLookAt({1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    math::float3 s{2.0f, 3.0f, 4.0f};

    math::float4x4 m = math::trs(t, r, s);

    math::float3 p{0.5f, -1.0f, 2.0f};
    math::float3 expected = t + r * (s * p);
    math::float4 got = m * math::float4(p, 1.0f);

    CHECK(std::abs(got.x - expected.x) < 1e-5f);
    CHECK(std::abs(got.y - expected.y) < 1e-5f);
    CHECK(std::abs(got.z - expected.z) < 1e-5f);
    CHECK(std::abs(got.w - 1.0f) < 1e-5f);
}

TEST_CASE("decomposeTrs round-trips a TRS matrix") {
    math::float3 t{1.0f, -2.0f, 0.5f};
    math::quat r = math::quatLookAt({0.3f, -0.4f, -1.0f}, {0.0f, 1.0f, 0.0f});
    math::float3 s{2.0f, 0.5f, 3.0f};

    math::Trs out = math::decomposeTrs(math::trs(t, r, s));

    CHECK(std::abs(out.translation.x - t.x) < 1e-5f);
    CHECK(std::abs(out.translation.y - t.y) < 1e-5f);
    CHECK(std::abs(out.translation.z - t.z) < 1e-5f);
    CHECK(std::abs(out.scale.x - s.x) < 1e-5f);
    CHECK(std::abs(out.scale.y - s.y) < 1e-5f);
    CHECK(std::abs(out.scale.z - s.z) < 1e-5f);
    // q and -q encode the same rotation.
    const float dot = std::abs(out.rotation.w * r.w + out.rotation.x * r.x + out.rotation.y * r.y +
                               out.rotation.z * r.z);
    CHECK(std::abs(dot - 1.0f) < 1e-5f);
}
