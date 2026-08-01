#include <kumo/asset/procedural_sky.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace kumo::asset {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float smoothstep(float edge0, float edge1, float x) {
    if (edge0 >= edge1) {
        return x < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

math::float3 lerp(const math::float3& a, const math::float3& b, float t) {
    return a + (b - a) * t;
}

// Inverts ibl_equirect_to_cube.comp's direction -> equirect uv mapping, so the
// direction returned here is exactly what that shader would sample at (x, y).
math::float3 equirectDirection(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                               std::uint32_t height) {
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
    const float phi = (u - 0.5f) * 2.0f * kPi;
    const float polar = v * kPi; // 0 at the zenith (row 0), pi at the nadir
    const float sinPolar = std::sin(polar);
    return {sinPolar * std::cos(phi), std::cos(polar), sinPolar * std::sin(phi)};
}

} // namespace

HdrImage proceduralSky(const ProceduralSkyDesc& desc) {
    HdrImage image;
    image.width = std::max(desc.width, 1u);
    image.height = std::max(desc.height, 1u);
    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0.0f);

    math::float3 sunDir = desc.sunDirection;
    float sunLen = math::length(sunDir);
    if (sunLen <= 1e-6f) {
        sunDir = math::float3(-0.4f, -0.7f, -0.5f);
        sunLen = math::length(sunDir);
    }
    const math::float3 towardSun = -sunDir / sunLen;
    const float sunRadiusRad = std::max(math::radians(desc.sunAngularRadiusDeg), 0.0f);
    const float sunEdgeStart = sunRadiusRad * 0.8f; // soft edge over ~20% of the radius

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const math::float3 dir = equirectDirection(x, y, image.width, image.height);

            math::float3 color =
                dir.y >= 0.0f
                    ? lerp(desc.horizonColor, desc.zenithColor, smoothstep(0.0f, 1.0f, dir.y))
                    : lerp(desc.horizonColor, desc.groundColor, smoothstep(0.0f, 1.0f, -dir.y));

            if (sunRadiusRad > 0.0f) {
                const float cosAngle = std::clamp(math::dot(dir, towardSun), -1.0f, 1.0f);
                const float angle = std::acos(cosAngle);
                const float sunFactor = 1.0f - smoothstep(sunEdgeStart, sunRadiusRad, angle);
                if (sunFactor > 0.0f) {
                    color = color + desc.sunColor * desc.sunIntensity * sunFactor;
                }
            }

            color = color * desc.exposure;

            const std::size_t idx = (static_cast<std::size_t>(y) * image.width + x) * 4;
            image.rgba[idx + 0] = color.x;
            image.rgba[idx + 1] = color.y;
            image.rgba[idx + 2] = color.z;
            image.rgba[idx + 3] = 1.0f;
        }
    }
    return image;
}

} // namespace kumo::asset
