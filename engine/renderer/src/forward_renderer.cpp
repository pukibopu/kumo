#include <kumo/renderer/forward_renderer.h>

#include "shader_load.h"

#include <kumo/core/assert.h>
#include <kumo/core/log.h>
#include <kumo/math/math.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace kumo::renderer {

namespace {

constexpr std::uint32_t kSampleCount = 4;
constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::RGBA16Float;
constexpr rhi::TextureFormat kDepthFormat = rhi::TextureFormat::Depth32Float;

struct GpuLight {
    math::float4 positionType;
    math::float4 colorIntensity;
    math::float4 directionRange;
};

// std140 mirror of FrameUniforms in shaders/include/common.glsl.
struct FrameUniformsData {
    math::float4x4 view;
    math::float4x4 proj;
    math::float4 cameraPos;
    math::float4 materialOverride;
    GpuLight lights[scene::Scene::kMaxLights];
    std::int32_t lightCount = 0;
    float prefilteredMipCount = 1.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
};
static_assert(sizeof(FrameUniformsData) == 944);

struct PerDrawData {
    math::float4x4 model;
    math::float4x4 normalMatrix;
};
static_assert(sizeof(PerDrawData) == 128);

// std140 mirror of MaterialFactors in pbr.frag.
struct MaterialFactorsData {
    math::float4 baseColor{1.0f};
    math::float4 metallicRoughness{1.0f, 1.0f, 0.0f, 0.0f};
    math::float4 emissive{0.0f};
};

MaterialFactorsData toFactors(const ForwardRenderer::MaterialParams& params) {
    return {
        .baseColor = {params.baseColor[0], params.baseColor[1], params.baseColor[2],
                      params.baseColor[3]},
        .metallicRoughness = {params.metallic, params.roughness, 0.0f, 0.0f},
        .emissive = {params.emissive[0], params.emissive[1], params.emissive[2], 0.0f},
    };
}

ForwardRenderer::MaterialParams toParams(const asset::MaterialData& mat) {
    ForwardRenderer::MaterialParams params;
    for (int c = 0; c < 4; ++c) {
        params.baseColor[c] = mat.baseColor[c];
    }
    params.metallic = mat.metallic;
    params.roughness = mat.roughness;
    for (int c = 0; c < 3; ++c) {
        params.emissive[c] = mat.emissive[c];
    }
    return params;
}

std::uint32_t fullMipChain(std::uint32_t width, std::uint32_t height) {
    std::uint32_t size = std::max(width, height);
    std::uint32_t mips = 1;
    while (size > 1) {
        size >>= 1;
        ++mips;
    }
    return mips;
}

} // namespace

bool ForwardRenderer::init(rhi::Device& device, rhi::TextureFormat outputFormat) {
    device_ = &device;
    outputFormat_ = outputFormat;

    materialSampler_ = device.createSampler({.maxAnisotropy = 8});
    iblSampler_ = device.createSampler({
        .addressModeU = rhi::AddressMode::ClampToEdge,
        .addressModeV = rhi::AddressMode::ClampToEdge,
        .addressModeW = rhi::AddressMode::ClampToEdge,
    });
    tonemapSampler_ = device.createSampler({
        .addressModeU = rhi::AddressMode::ClampToEdge,
        .addressModeV = rhi::AddressMode::ClampToEdge,
        .addressModeW = rhi::AddressMode::ClampToEdge,
    });
    if (!materialSampler_ || !iblSampler_ || !tonemapSampler_) {
        return false;
    }

    defaultWhite_ = makeSolidTexture(255, 255, 255, 255);
    defaultNormal_ = makeSolidTexture(128, 128, 255, 255);
    if (!defaultWhite_ || !defaultNormal_) {
        return false;
    }

    if (!buildPipelines(true)) {
        return false;
    }

    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        frameUniforms_[slot] = device.createBuffer({
            .size = sizeof(FrameUniformsData),
            .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
        });
        if (!frameUniforms_[slot]) {
            return false;
        }
        frameGroups_[slot] = device.createBindGroup({
            .layout = frameLayout_,
            .entries = {{.binding = 0, .buffer = frameUniforms_[slot]}},
        });
        if (!frameGroups_[slot]) {
            return false;
        }
    }
    return true;
}

rhi::Ptr<rhi::Texture> ForwardRenderer::makeSolidTexture(std::uint8_t r, std::uint8_t g,
                                                         std::uint8_t b, std::uint8_t a) {
    KUMO_ASSERT(device_ != nullptr);
    rhi::Ptr<rhi::Texture> texture = device_->createTexture({
        .size = {1, 1},
        .format = rhi::TextureFormat::RGBA8Unorm,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
    });
    if (texture) {
        const std::uint8_t pixel[4] = {r, g, b, a};
        device_->queue().writeTexture(*texture, pixel, 4, {1, 1});
    }
    return texture;
}

bool ForwardRenderer::uploadMesh(const asset::MeshData& mesh, GpuMesh& out) {
    KUMO_ASSERT(device_ != nullptr);
    const std::uint64_t vertexBytes = mesh.vertices.size() * sizeof(asset::Vertex);
    const std::uint64_t indexBytes = mesh.indices.size() * sizeof(std::uint32_t);
    out.vertexBuffer = device_->createBuffer(
        {.size = vertexBytes, .usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst});
    out.indexBuffer = device_->createBuffer(
        {.size = indexBytes, .usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst});
    if (!out.vertexBuffer || !out.indexBuffer) {
        return false;
    }
    device_->queue().writeBuffer(*out.vertexBuffer, 0, mesh.vertices.data(), vertexBytes);
    device_->queue().writeBuffer(*out.indexBuffer, 0, mesh.indices.data(), indexBytes);
    out.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    out.localAabb = mesh.localAabb;
    return true;
}

bool ForwardRenderer::appendMaterial(const MaterialParams& params,
                                     const MaterialTextures& textures) {
    KUMO_ASSERT(device_ != nullptr);
    const MaterialFactorsData factors = toFactors(params);
    rhi::Ptr<rhi::Buffer> buffer = device_->createBuffer({
        .size = sizeof(MaterialFactorsData),
        .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
    });
    if (!buffer) {
        return false;
    }
    device_->queue().writeBuffer(*buffer, 0, &factors, sizeof(factors));
    rhi::Ptr<rhi::BindGroup> group = device_->createBindGroup({
        .layout = materialLayout_,
        .entries = {{.binding = 0, .texture = textures.baseColor},
                    {.binding = 1, .texture = textures.metallicRoughness},
                    {.binding = 2, .texture = textures.normal},
                    {.binding = 3, .texture = textures.occlusion},
                    {.binding = 4, .texture = textures.emissive},
                    {.binding = 5, .sampler = materialSampler_},
                    {.binding = 6, .buffer = buffer}},
    });
    if (!group) {
        return false;
    }
    materialFactorBuffers_.push_back(std::move(buffer));
    materialGroups_.push_back(std::move(group));
    materialParams_.push_back(params);
    return true;
}

std::int32_t ForwardRenderer::addMesh(const asset::MeshData& mesh) {
    KUMO_ASSERT(device_ != nullptr);
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return -1;
    }
    GpuMesh gpu;
    if (!uploadMesh(mesh, gpu)) {
        return -1;
    }
    meshes_.push_back(std::move(gpu));
    return static_cast<std::int32_t>(meshes_.size() - 1);
}

std::int32_t ForwardRenderer::addMaterial(const MaterialParams& params) {
    KUMO_ASSERT(device_ != nullptr);
    if (!materialLayout_ || !defaultWhite_ || !defaultNormal_) {
        return -1;
    }
    if (!appendMaterial(params, {.baseColor = defaultWhite_,
                                 .metallicRoughness = defaultWhite_,
                                 .normal = defaultNormal_,
                                 .occlusion = defaultWhite_,
                                 .emissive = defaultWhite_})) {
        return -1;
    }
    return static_cast<std::int32_t>(materialGroups_.size() - 1);
}

std::uint32_t ForwardRenderer::meshCount() const {
    return static_cast<std::uint32_t>(meshes_.size());
}

const math::Aabb* ForwardRenderer::meshLocalAabb(std::uint32_t index) const {
    return index < meshes_.size() ? &meshes_[index].localAabb : nullptr;
}

std::uint32_t ForwardRenderer::materialCount() const {
    return static_cast<std::uint32_t>(materialGroups_.size());
}

std::uint32_t ForwardRenderer::defaultMaterialIndex() const {
    return static_cast<std::uint32_t>(defaultMaterialIndex_);
}

const ForwardRenderer::MaterialParams* ForwardRenderer::materialParams(std::uint32_t index) const {
    return index < materialParams_.size() ? &materialParams_[index] : nullptr;
}

bool ForwardRenderer::setMaterialParams(std::uint32_t index, const MaterialParams& params) {
    KUMO_ASSERT(device_ != nullptr);
    if (index >= materialParams_.size()) {
        return false;
    }
    materialParams_[index] = params;
    const MaterialFactorsData factors = toFactors(params);
    // Single-buffered material UBO: overwriting it can tear a frame still in
    // flight, which at worst shows one frame of mixed factors.
    device_->queue().writeBuffer(*materialFactorBuffers_[index], 0, &factors, sizeof(factors));
    return true;
}

bool ForwardRenderer::buildPipelines(bool deriveLayouts) {
    KUMO_ASSERT(device_ != nullptr);
    auto pbrVert = detail::loadStage(*device_, "pbr.vert", shaderc::Stage::Vertex);
    auto pbrFrag = detail::loadStage(*device_, "pbr.frag", shaderc::Stage::Fragment);
    auto skyboxVert = detail::loadStage(*device_, "skybox.vert", shaderc::Stage::Vertex);
    auto skyboxFrag = detail::loadStage(*device_, "skybox.frag", shaderc::Stage::Fragment);
    auto fullscreenVert = detail::loadStage(*device_, "fullscreen.vert", shaderc::Stage::Vertex);
    auto tonemapFrag = detail::loadStage(*device_, "tonemap.frag", shaderc::Stage::Fragment);
    if (!pbrVert || !pbrFrag || !skyboxVert || !skyboxFrag || !fullscreenVert || !tonemapFrag) {
        return false;
    }

    const detail::StageReflection pbrStages[] = {
        {&pbrVert->reflection, rhi::ShaderStage::Vertex},
        {&pbrFrag->reflection, rhi::ShaderStage::Fragment}};
    const detail::StageReflection skyboxStages[] = {
        {&skyboxVert->reflection, rhi::ShaderStage::Vertex},
        {&skyboxFrag->reflection, rhi::ShaderStage::Fragment}};
    const detail::StageReflection tonemapStages[] = {
        {&fullscreenVert->reflection, rhi::ShaderStage::Vertex},
        {&tonemapFrag->reflection, rhi::ShaderStage::Fragment}};

    const std::string signature = detail::layoutSignature(pbrStages) + "|" +
                                  detail::layoutSignature(skyboxStages) + "|" +
                                  detail::layoutSignature(tonemapStages);
    if (deriveLayouts) {
        auto pbrLayouts = detail::layoutsFromReflection(*device_, pbrStages);
        auto skyboxLayouts = detail::layoutsFromReflection(*device_, skyboxStages);
        auto tonemapLayouts = detail::layoutsFromReflection(*device_, tonemapStages);
        if (pbrLayouts.size() < 3 || !pbrLayouts[0] || !pbrLayouts[1] || !pbrLayouts[2] ||
            skyboxLayouts.size() < 2 || !skyboxLayouts[1] || tonemapLayouts.size() < 2 ||
            !tonemapLayouts[1]) {
            logError("shader binding layout does not match the set 0/1/2 convention");
            return false;
        }
        frameLayout_ = pbrLayouts[0];
        materialLayout_ = pbrLayouts[1];
        iblLayout_ = pbrLayouts[2];
        skyboxLayout_ = skyboxLayouts[1];
        tonemapLayout_ = tonemapLayouts[1];
        layoutSignature_ = signature;
    } else if (signature != layoutSignature_) {
        // Existing bind groups were created against the original layouts; a
        // binding-table change needs a restart, not a hot reload.
        logError("shader reload changed the binding layout; keeping current pipelines");
        return false;
    }

    const std::uint32_t pushSize =
        std::max(pbrVert->reflection.pushConstantSize, pbrFrag->reflection.pushConstantSize);
    rhi::Ptr<rhi::RenderPipeline> pbr = device_->createRenderPipeline({
        .vertexShader = pbrVert->module,
        .fragmentShader = pbrFrag->module,
        .vertexBuffers =
            {{.stride = sizeof(asset::Vertex),
              .attributes =
                  {{.format = rhi::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
                   {.format = rhi::VertexFormat::Float32x3, .offset = 12, .shaderLocation = 1},
                   {.format = rhi::VertexFormat::Float32x4, .offset = 24, .shaderLocation = 2},
                   {.format = rhi::VertexFormat::Float32x2, .offset = 40, .shaderLocation = 3}}}},
        .bindGroupLayouts = {frameLayout_, materialLayout_, iblLayout_},
        .pushConstantSize = pushSize,
        .colorFormats = {kHdrFormat},
        .depthStencil = {.format = kDepthFormat,
                         .depthWriteEnabled = true,
                         .depthCompare = rhi::CompareFunction::GreaterEqual},
        .cullMode = rhi::CullMode::Back,
        .sampleCount = kSampleCount,
    });
    rhi::Ptr<rhi::RenderPipeline> skybox = device_->createRenderPipeline({
        .vertexShader = skyboxVert->module,
        .fragmentShader = skyboxFrag->module,
        .bindGroupLayouts = {frameLayout_, skyboxLayout_},
        .colorFormats = {kHdrFormat},
        .depthStencil = {.format = kDepthFormat,
                         .depthWriteEnabled = false,
                         .depthCompare = rhi::CompareFunction::GreaterEqual},
        .sampleCount = kSampleCount,
    });
    rhi::Ptr<rhi::RenderPipeline> tonemap = device_->createRenderPipeline({
        .vertexShader = fullscreenVert->module,
        .fragmentShader = tonemapFrag->module,
        .bindGroupLayouts = {nullptr, tonemapLayout_},
        .colorFormats = {outputFormat_},
    });
    if (!pbr || !skybox || !tonemap) {
        return false;
    }
    pbrPipeline_ = std::move(pbr);
    skyboxPipeline_ = std::move(skybox);
    tonemapPipeline_ = std::move(tonemap);
    return true;
}

bool ForwardRenderer::reloadPipelines() {
    return buildPipelines(false);
}

bool ForwardRenderer::loadScene(const asset::SceneAsset& sceneAsset,
                                const ibl::Environment& environment) {
    KUMO_ASSERT(device_ != nullptr);
    if (!environment.valid()) {
        logError("loadScene: invalid IBL environment");
        return false;
    }
    environment_ = environment;

    iblGroup_ = device_->createBindGroup({
        .layout = iblLayout_,
        .entries = {{.binding = 0, .texture = environment_.irradiance},
                    {.binding = 1, .texture = environment_.prefiltered},
                    {.binding = 2, .texture = environment_.brdfLut},
                    {.binding = 3, .sampler = iblSampler_}},
    });
    skyboxGroup_ = device_->createBindGroup({
        .layout = skyboxLayout_,
        .entries = {{.binding = 0, .texture = environment_.environment},
                    {.binding = 1, .sampler = iblSampler_}},
    });
    if (!iblGroup_ || !skyboxGroup_) {
        return false;
    }

    meshes_.clear();
    for (const asset::MeshData& mesh : sceneAsset.meshes) {
        GpuMesh gpu;
        if (!uploadMesh(mesh, gpu)) {
            return false;
        }
        meshes_.push_back(std::move(gpu));
    }

    rhi::Ptr<rhi::CommandEncoder> encoder = device_->queue().createCommandEncoder();

    textures_.clear();
    for (const asset::TextureData& tex : sceneAsset.textures) {
        rhi::Ptr<rhi::Texture> texture = device_->createTexture({
            .size = {tex.width, tex.height},
            .format =
                tex.srgb ? rhi::TextureFormat::RGBA8UnormSrgb : rhi::TextureFormat::RGBA8Unorm,
            .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc |
                     rhi::TextureUsage::CopyDst,
            .mipLevelCount = fullMipChain(tex.width, tex.height),
        });
        if (!texture) {
            return false;
        }
        device_->queue().writeTexture(*texture, tex.rgba.data(),
                                      static_cast<std::uint64_t>(tex.width) * 4,
                                      {tex.width, tex.height});
        encoder->generateMipmaps(*texture);
        textures_.push_back(std::move(texture));
    }

    auto textureOr = [&](std::int32_t index, const rhi::Ptr<rhi::Texture>& fallback) {
        return index >= 0 && static_cast<std::size_t>(index) < textures_.size()
                   ? textures_[static_cast<std::size_t>(index)]
                   : fallback;
    };

    materialFactorBuffers_.clear();
    materialGroups_.clear();
    materialParams_.clear();
    for (const asset::MaterialData& mat : sceneAsset.materials) {
        const MaterialTextures textures{.baseColor = textureOr(mat.baseColorTexture, defaultWhite_),
                                        .metallicRoughness =
                                            textureOr(mat.metallicRoughnessTexture, defaultWhite_),
                                        .normal = textureOr(mat.normalTexture, defaultNormal_),
                                        .occlusion = textureOr(mat.occlusionTexture, defaultWhite_),
                                        .emissive = textureOr(mat.emissiveTexture, defaultWhite_)};
        if (!appendMaterial(toParams(mat), textures)) {
            return false;
        }
    }
    defaultMaterialIndex_ = materialGroups_.size();
    if (addMaterial(MaterialParams{}) < 0) {
        return false;
    }

    encoder->finishAndSubmit();
    device_->queue().waitIdle();
    return true;
}

void ForwardRenderer::resize(rhi::Extent2D size) {
    KUMO_ASSERT(device_ != nullptr);
    if (size.width == 0 || size.height == 0) {
        return;
    }
    if (hdrMsaa_ && size.width == size_.width && size.height == size_.height) {
        return;
    }
    // The old targets may still be referenced by an in-flight frame.
    device_->queue().waitIdle();
    size_ = size;
    hdrMsaa_ = device_->createTexture({
        .size = size,
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::RenderTarget,
        .sampleCount = kSampleCount,
    });
    hdrResolve_ = device_->createTexture({
        .size = size,
        .format = kHdrFormat,
        .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
    });
    depth_ = device_->createTexture({
        .size = size,
        .format = kDepthFormat,
        .usage = rhi::TextureUsage::RenderTarget,
        .sampleCount = kSampleCount,
    });
    tonemapGroup_ = device_->createBindGroup({
        .layout = tonemapLayout_,
        .entries = {{.binding = 0, .texture = hdrResolve_},
                    {.binding = 1, .sampler = tonemapSampler_}},
    });
}

void ForwardRenderer::setMaterialOverride(float metallic, float roughness) {
    overrideMetallic_ = metallic;
    overrideRoughness_ = roughness;
}

void ForwardRenderer::updateFrameUniforms(const scene::Scene& scene) {
    FrameUniformsData data;
    const float aspect =
        static_cast<float>(size_.width) / static_cast<float>(std::max(1u, size_.height));
    data.view = scene.camera.view();
    data.proj = scene.camera.projection(aspect);
    data.cameraPos = math::float4(scene.camera.position, 1.0f);
    data.materialOverride = {overrideMetallic_, overrideRoughness_, 0.0f, 0.0f};

    const std::span<const scene::Light> lights = scene.lights();
    data.lightCount = static_cast<std::int32_t>(lights.size());
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const scene::Light& light = lights[i];
        data.lights[i].positionType =
            math::float4(light.position, light.type == scene::LightType::Point ? 1.0f : 0.0f);
        data.lights[i].colorIntensity = math::float4(light.color, light.intensity);
        data.lights[i].directionRange = math::float4(math::normalize(light.direction), light.range);
    }
    data.prefilteredMipCount = static_cast<float>(std::max(1u, environment_.prefilteredMips));

    device_->queue().writeBuffer(*frameUniforms_[frameSlot_], 0, &data, sizeof(data));
}

void ForwardRenderer::render(rhi::CommandEncoder& encoder, const scene::Scene& scene,
                             rhi::Texture* output, const Overlay& overlay) {
    KUMO_ASSERT(device_ != nullptr);
    if (output == nullptr || !pbrPipeline_ || !hdrMsaa_ ||
        defaultMaterialIndex_ >= materialGroups_.size()) {
        return;
    }
    // Two uniform slots track the two frames in flight, so this write never
    // races the buffer the GPU is still reading.
    frameSlot_ = (frameSlot_ + 1) % kFrameSlots;
    updateFrameUniforms(scene);

    rhi::RenderPassEncoder& scenePass = encoder.beginRenderPass({
        .colorAttachments = {{.texture = hdrMsaa_.get(),
                              .loadOp = rhi::LoadOp::Clear,
                              .storeOp = rhi::StoreOp::DontCare,
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .resolveTarget = hdrResolve_.get()}},
        .depthAttachment = {.texture = depth_.get(),
                            .loadOp = rhi::LoadOp::Clear,
                            .storeOp = rhi::StoreOp::DontCare,
                            .clearDepth = 0.0f},
    });
    scenePass.setPipeline(*pbrPipeline_);
    scenePass.setBindGroup(0, *frameGroups_[frameSlot_]);
    scenePass.setBindGroup(2, *iblGroup_);
    scene.entities.forEach([&](scene::EntityId, const scene::Entity& entity) {
        if (entity.meshIndex < 0 || static_cast<std::size_t>(entity.meshIndex) >= meshes_.size()) {
            return;
        }
        const GpuMesh& mesh = meshes_[static_cast<std::size_t>(entity.meshIndex)];
        const std::size_t materialIndex =
            entity.materialIndex >= 0 &&
                    static_cast<std::size_t>(entity.materialIndex) < materialGroups_.size()
                ? static_cast<std::size_t>(entity.materialIndex)
                : defaultMaterialIndex_;
        scenePass.setBindGroup(1, *materialGroups_[materialIndex]);

        PerDrawData draw;
        draw.model = entity.transform.matrix();
        draw.normalMatrix = math::transpose(math::inverse(draw.model));
        scenePass.setPushConstants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, &draw,
                                   sizeof(draw));
        scenePass.setVertexBuffer(0, *mesh.vertexBuffer);
        scenePass.setIndexBuffer(*mesh.indexBuffer, rhi::IndexFormat::Uint32);
        scenePass.drawIndexed(mesh.indexCount);
    });
    if (skyboxGroup_) {
        scenePass.setPipeline(*skyboxPipeline_);
        scenePass.setBindGroup(0, *frameGroups_[frameSlot_]);
        scenePass.setBindGroup(1, *skyboxGroup_);
        scenePass.draw(3);
    }
    scenePass.end();

    rhi::RenderPassEncoder& tonemapPass = encoder.beginRenderPass({
        .colorAttachments = {{.texture = output,
                              .loadOp = rhi::LoadOp::Clear,
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}}},
    });
    tonemapPass.setPipeline(*tonemapPipeline_);
    tonemapPass.setBindGroup(1, *tonemapGroup_);
    tonemapPass.draw(3);
    if (overlay) {
        overlay(tonemapPass);
    }
    tonemapPass.end();
}

} // namespace kumo::renderer
