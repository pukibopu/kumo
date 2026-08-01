#pragma once

#include <kumo/asset/asset.h>
#include <kumo/math/math.h>

#include <cstdint>

namespace kumo::asset {

// Deterministic analytic sky, baked to the same equirect layout loadHdr
// produces so it can feed ibl::bake without a renderer-side special case.
struct ProceduralSkyDesc {
    math::float3 zenithColor{0.18f, 0.32f, 0.62f};
    math::float3 horizonColor{0.72f, 0.78f, 0.85f};
    math::float3 groundColor{0.22f, 0.20f, 0.18f};
    // Direction the sunlight travels, like scene::Light::direction; the sun
    // disc is drawn toward -sunDirection.
    math::float3 sunDirection{-0.4f, -0.7f, -0.5f};
    math::float3 sunColor{1.0f, 0.95f, 0.85f};
    float sunIntensity = 60.0f;
    float sunAngularRadiusDeg = 1.5f;
    float exposure = 1.0f;
    std::uint32_t width = 512;
    std::uint32_t height = 256;
    bool operator==(const ProceduralSkyDesc&) const = default;
};

// Analytic gradient sky + sun disc, in the same equirect layout/orientation as
// ibl_equirect_to_cube.comp expects (row 0 = zenith, +Y up). Deterministic: no
// randomness, same desc always produces byte-identical output.
HdrImage proceduralSky(const ProceduralSkyDesc& desc = {});

} // namespace kumo::asset
