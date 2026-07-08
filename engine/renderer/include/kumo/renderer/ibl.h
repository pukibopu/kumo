#pragma once

#include <kumo/asset/asset.h>
#include <kumo/rhi/rhi.h>

#include <cstdint>

namespace kumo::renderer::ibl {

template <typename T> using Ptr = rhi::Ptr<T>;

struct Environment {
    // Base environment cube (mipped); the skybox samples this at full detail.
    Ptr<rhi::Texture> environment;
    Ptr<rhi::Texture> irradiance;
    Ptr<rhi::Texture> prefiltered;
    Ptr<rhi::Texture> brdfLut;
    std::uint32_t prefilteredMips = 0;

    bool valid() const {
        return environment && irradiance && prefiltered && brdfLut;
    }
};

// Bakes irradiance/prefiltered/BRDF-LUT textures from an equirectangular HDR
// via compute; blocks until the GPU work completes. Empty result on failure.
Environment bake(rhi::Device& device, const asset::HdrImage& hdr);

} // namespace kumo::renderer::ibl
