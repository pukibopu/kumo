#include <kumo/renderer/ibl.h>

#include "shader_load.h"

#include <kumo/core/log.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace kumo::renderer::ibl {

namespace {

constexpr std::uint32_t kEnvironmentSize = 512;
constexpr std::uint32_t kIrradianceSize = 32;
constexpr std::uint32_t kPrefilteredSize = 128;
constexpr std::uint32_t kPrefilteredMips = 5;
constexpr std::uint32_t kBrdfLutSize = 512;

struct ComputeStep {
    rhi::Ptr<rhi::ComputePipeline> pipeline;
    rhi::Ptr<rhi::BindGroupLayout> layout;
};

std::optional<ComputeStep> buildStep(rhi::Device& device, const char* file) {
    auto stage = detail::loadStage(device, file, shaderc::Stage::Compute);
    if (!stage) {
        return std::nullopt;
    }
    const detail::StageReflection reflections[] = {
        {&stage->reflection, rhi::ShaderStage::Compute}};
    auto layouts = detail::layoutsFromReflection(device, reflections);
    if (layouts.empty() || !layouts[0]) {
        logError("{}: no set 0 bindings reflected", file);
        return std::nullopt;
    }
    rhi::Ptr<rhi::ComputePipeline> pipeline = device.createComputePipeline({
        .shader = stage->module,
        .bindGroupLayouts = {layouts[0]},
        .pushConstantSize = stage->reflection.pushConstantSize,
        .workgroupSizeX = stage->reflection.workgroupSize[0],
        .workgroupSizeY = stage->reflection.workgroupSize[1],
        .workgroupSizeZ = stage->reflection.workgroupSize[2],
    });
    if (!pipeline) {
        return std::nullopt;
    }
    return ComputeStep{std::move(pipeline), std::move(layouts[0])};
}

// Per-face (and per-mip) 2D storage views; layout tracking is per image, so
// every step reads one texture and writes a different one.
std::vector<rhi::Ptr<rhi::Texture>> faceViews(rhi::Device& device,
                                              const rhi::Ptr<rhi::Texture>& cube,
                                              std::uint32_t mip) {
    std::vector<rhi::Ptr<rhi::Texture>> views(6);
    for (std::uint32_t face = 0; face < 6; ++face) {
        views[face] = device.createTextureView(cube, {.baseMipLevel = mip,
                                                      .mipLevelCount = 1,
                                                      .baseArrayLayer = face,
                                                      .arrayLayerCount = 1,
                                                      .dimension = rhi::TextureDimension::Tex2D});
        if (!views[face]) {
            return {};
        }
    }
    return views;
}

std::uint32_t groupsFor(std::uint32_t size, std::uint32_t workgroup) {
    return (size + workgroup - 1) / workgroup;
}

std::uint32_t fullMipChain(std::uint32_t size) {
    std::uint32_t mips = 1;
    while (size > 1) {
        size >>= 1;
        ++mips;
    }
    return mips;
}

} // namespace

Environment bake(rhi::Device& device, const asset::HdrImage& hdr) {
    const auto start = std::chrono::steady_clock::now();
    if (hdr.width == 0 || hdr.height == 0) {
        logError("ibl::bake: empty HDR image");
        return {};
    }

    auto equirectStep = buildStep(device, "ibl_equirect_to_cube.comp");
    auto irradianceStep = buildStep(device, "ibl_irradiance.comp");
    auto prefilterStep = buildStep(device, "ibl_prefilter.comp");
    auto brdfStep = buildStep(device, "ibl_brdf_lut.comp");
    if (!equirectStep || !irradianceStep || !prefilterStep || !brdfStep) {
        return {};
    }

    rhi::Ptr<rhi::Texture> equirect = device.createTexture({
        .size = {hdr.width, hdr.height},
        .format = rhi::TextureFormat::RGBA32Float,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
    });
    Environment env;
    env.environment = device.createTexture({
        .size = {kEnvironmentSize, kEnvironmentSize},
        .format = rhi::TextureFormat::RGBA16Float,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage |
                 rhi::TextureUsage::CopySrc | rhi::TextureUsage::CopyDst,
        .dimension = rhi::TextureDimension::Cube,
        .mipLevelCount = fullMipChain(kEnvironmentSize),
    });
    env.irradiance = device.createTexture({
        .size = {kIrradianceSize, kIrradianceSize},
        .format = rhi::TextureFormat::RGBA16Float,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage,
        .dimension = rhi::TextureDimension::Cube,
    });
    env.prefiltered = device.createTexture({
        .size = {kPrefilteredSize, kPrefilteredSize},
        .format = rhi::TextureFormat::RGBA16Float,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage,
        .dimension = rhi::TextureDimension::Cube,
        .mipLevelCount = kPrefilteredMips,
    });
    env.brdfLut = device.createTexture({
        .size = {kBrdfLutSize, kBrdfLutSize},
        .format = rhi::TextureFormat::RG16Float,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage,
    });
    env.prefilteredMips = kPrefilteredMips;
    if (!equirect || !env.valid()) {
        return {};
    }

    rhi::Ptr<rhi::Sampler> sampler = device.createSampler({
        .addressModeU = rhi::AddressMode::ClampToEdge,
        .addressModeV = rhi::AddressMode::ClampToEdge,
        .addressModeW = rhi::AddressMode::ClampToEdge,
    });
    if (!sampler) {
        return {};
    }

    device.queue().writeTexture(*equirect, hdr.rgba.data(),
                                static_cast<std::uint64_t>(hdr.width) * 16,
                                {hdr.width, hdr.height});

    const auto envFaces = faceViews(device, env.environment, 0);
    const auto irradianceFaces = faceViews(device, env.irradiance, 0);
    if (envFaces.empty() || irradianceFaces.empty()) {
        return {};
    }

    auto makeGroup = [&](const rhi::Ptr<rhi::BindGroupLayout>& layout,
                         const rhi::Ptr<rhi::Texture>& source,
                         const rhi::Ptr<rhi::Texture>& dest) {
        return device.createBindGroup({.layout = layout,
                                       .entries = {{.binding = 0, .texture = source},
                                                   {.binding = 1, .sampler = sampler},
                                                   {.binding = 2, .texture = dest}}});
    };

    std::array<rhi::Ptr<rhi::BindGroup>, 6> equirectGroups;
    std::array<rhi::Ptr<rhi::BindGroup>, 6> irradianceGroups;
    for (std::uint32_t face = 0; face < 6; ++face) {
        equirectGroups[face] = makeGroup(equirectStep->layout, equirect, envFaces[face]);
        irradianceGroups[face] =
            makeGroup(irradianceStep->layout, env.environment, irradianceFaces[face]);
        if (!equirectGroups[face] || !irradianceGroups[face]) {
            return {};
        }
    }

    std::vector<rhi::Ptr<rhi::BindGroup>> prefilterGroups(kPrefilteredMips * 6);
    for (std::uint32_t mip = 0; mip < kPrefilteredMips; ++mip) {
        const auto mipFaces = faceViews(device, env.prefiltered, mip);
        if (mipFaces.empty()) {
            return {};
        }
        for (std::uint32_t face = 0; face < 6; ++face) {
            prefilterGroups[mip * 6 + face] =
                makeGroup(prefilterStep->layout, env.environment, mipFaces[face]);
            if (!prefilterGroups[mip * 6 + face]) {
                return {};
            }
        }
    }
    rhi::Ptr<rhi::BindGroup> brdfGroup = device.createBindGroup(
        {.layout = brdfStep->layout, .entries = {{.binding = 0, .texture = env.brdfLut}}});
    if (!brdfGroup) {
        return {};
    }

    rhi::Ptr<rhi::CommandEncoder> encoder = device.queue().createCommandEncoder();
    {
        rhi::ComputePassEncoder& pass = encoder->beginComputePass();
        pass.setPipeline(*equirectStep->pipeline);
        const std::uint32_t groups = groupsFor(kEnvironmentSize, 8);
        for (std::int32_t face = 0; face < 6; ++face) {
            pass.setBindGroup(0, *equirectGroups[static_cast<std::size_t>(face)]);
            pass.setPushConstants(&face, sizeof(face));
            pass.dispatch(groups, groups);
        }
        pass.end();
    }
    encoder->generateMipmaps(*env.environment);
    {
        rhi::ComputePassEncoder& pass = encoder->beginComputePass();
        pass.setPipeline(*irradianceStep->pipeline);
        const std::uint32_t irradianceGroupCount = groupsFor(kIrradianceSize, 8);
        for (std::int32_t face = 0; face < 6; ++face) {
            pass.setBindGroup(0, *irradianceGroups[static_cast<std::size_t>(face)]);
            pass.setPushConstants(&face, sizeof(face));
            pass.dispatch(irradianceGroupCount, irradianceGroupCount);
        }

        pass.setPipeline(*prefilterStep->pipeline);
        for (std::uint32_t mip = 0; mip < kPrefilteredMips; ++mip) {
            struct {
                std::int32_t face;
                float roughness;
                float sourceResolution;
            } push{0, static_cast<float>(mip) / static_cast<float>(kPrefilteredMips - 1),
                   static_cast<float>(kEnvironmentSize)};
            const std::uint32_t mipSize = kPrefilteredSize >> mip;
            const std::uint32_t mipGroups = groupsFor(mipSize, 8);
            for (std::int32_t face = 0; face < 6; ++face) {
                push.face = face;
                pass.setBindGroup(0, *prefilterGroups[mip * 6 + static_cast<std::size_t>(face)]);
                pass.setPushConstants(&push, sizeof(push));
                pass.dispatch(mipGroups, mipGroups);
            }
        }

        pass.setPipeline(*brdfStep->pipeline);
        pass.setBindGroup(0, *brdfGroup);
        const std::uint32_t lutGroups = groupsFor(kBrdfLutSize, 8);
        pass.dispatch(lutGroups, lutGroups);
        pass.end();
    }
    encoder->finishAndSubmit();
    device.queue().waitIdle();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    logInfo("IBL bake: {}x{} HDR -> env {}, irradiance {}, prefiltered {} ({} mips), {} ms",
            hdr.width, hdr.height, kEnvironmentSize, kIrradianceSize, kPrefilteredSize,
            kPrefilteredMips, elapsed.count());
    return env;
}

} // namespace kumo::renderer::ibl
