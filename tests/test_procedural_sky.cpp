#include <doctest/doctest.h>

#include <kumo/asset/procedural_sky.h>
#include <kumo/math/math.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace kumo;

namespace {

float luminance(const asset::HdrImage& image, std::size_t texel) {
    const float r = image.rgba[texel * 4 + 0];
    const float g = image.rgba[texel * 4 + 1];
    const float b = image.rgba[texel * 4 + 2];
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

math::float3 texelColor(const asset::HdrImage& image, std::size_t texel) {
    return {image.rgba[texel * 4 + 0], image.rgba[texel * 4 + 1], image.rgba[texel * 4 + 2]};
}

} // namespace

TEST_CASE("proceduralSky produces the requested dimensions and pixel count") {
    asset::ProceduralSkyDesc desc;
    desc.width = 64;
    desc.height = 32;
    const asset::HdrImage image = asset::proceduralSky(desc);
    CHECK(image.width == 64);
    CHECK(image.height == 32);
    CHECK(image.rgba.size() == 64u * 32u * 4u);
}

TEST_CASE("proceduralSky is deterministic") {
    asset::ProceduralSkyDesc desc;
    const asset::HdrImage a = asset::proceduralSky(desc);
    const asset::HdrImage b = asset::proceduralSky(desc);
    REQUIRE(a.rgba.size() == b.rgba.size());
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        CHECK(a.rgba[i] == b.rgba[i]);
    }
}

TEST_CASE("proceduralSky puts the brightest texel at the toward-sun direction") {
    asset::ProceduralSkyDesc desc;
    desc.width = 128;
    desc.height = 64;
    const asset::HdrImage image = asset::proceduralSky(desc);

    std::size_t brightest = 0;
    float bestLuminance = -1.0f;
    for (std::size_t texel = 0; texel < static_cast<std::size_t>(image.width) * image.height;
         ++texel) {
        const float l = luminance(image, texel);
        if (l > bestLuminance) {
            bestLuminance = l;
            brightest = texel;
        }
    }

    const std::uint32_t x = static_cast<std::uint32_t>(brightest % image.width);
    const std::uint32_t y = static_cast<std::uint32_t>(brightest / image.width);
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
    constexpr float kPi = 3.14159265358979323846f;
    const float phi = (u - 0.5f) * 2.0f * kPi;
    const float polar = v * kPi;
    const math::float3 dir{std::sin(polar) * std::cos(phi), std::cos(polar),
                           std::sin(polar) * std::sin(phi)};

    const math::float3 towardSun = math::normalize(-desc.sunDirection);
    CHECK(math::dot(dir, towardSun) > 0.99f);
}

TEST_CASE("proceduralSky rows shade from ground at the bottom to zenith at the top") {
    asset::ProceduralSkyDesc desc;
    desc.width = 32;
    desc.height = 32;
    // No sun disc so the gradient check is not skewed by the sun's brightness.
    desc.sunIntensity = 0.0f;
    const asset::HdrImage image = asset::proceduralSky(desc);

    const std::size_t x = 0;
    const math::float3 top = texelColor(image, x);
    const math::float3 bottom =
        texelColor(image, x + static_cast<std::size_t>(image.height - 1) * image.width);

    const float topToZenith = math::length(top - desc.zenithColor);
    const float topToGround = math::length(top - desc.groundColor);
    CHECK(topToZenith < topToGround);

    const float bottomToGround = math::length(bottom - desc.groundColor);
    const float bottomToZenith = math::length(bottom - desc.zenithColor);
    CHECK(bottomToGround < bottomToZenith);
}

TEST_CASE("proceduralSky output is always finite") {
    asset::ProceduralSkyDesc desc;
    desc.width = 32;
    desc.height = 16;
    const asset::HdrImage image = asset::proceduralSky(desc);
    for (float v : image.rgba) {
        CHECK(std::isfinite(v));
    }
}

TEST_CASE("proceduralSky falls back to a default direction on a zero sun vector") {
    asset::ProceduralSkyDesc desc;
    desc.sunDirection = {0.0f, 0.0f, 0.0f};
    // A generous angular radius guarantees some texel lands inside the disc
    // even at this low a resolution; the point of this case is the NaN-free
    // fallback, not disc-detection precision (covered elsewhere at higher res).
    desc.sunAngularRadiusDeg = 20.0f;
    desc.width = 32;
    desc.height = 16;
    const asset::HdrImage image = asset::proceduralSky(desc);
    for (float v : image.rgba) {
        CHECK(std::isfinite(v));
    }
    // A visible sun disc still appears somewhere (not degenerate/all-zero).
    float maxLuminance = 0.0f;
    for (std::size_t texel = 0; texel < static_cast<std::size_t>(image.width) * image.height;
         ++texel) {
        maxLuminance = std::max(maxLuminance, luminance(image, texel));
    }
    CHECK(maxLuminance > 1.0f);
}
