#include <kumo/renderer/forward_renderer.h>

#include "shader_load.h"

#include <kumo/core/assert.h>
#include <kumo/core/log.h>
#include <kumo/math/math.h>

#include "shadow.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <utility>

namespace kumo::renderer {

namespace {

constexpr std::uint32_t kSampleCount = 4;
constexpr gpu::TextureFormat kHdrFormat = gpu::TextureFormat::RGBA16Float;
constexpr gpu::TextureFormat kDepthFormat = gpu::TextureFormat::Depth32Float;
constexpr std::uint32_t kShadowMapSize = 2048;
// Slightly larger than a single shadow-map texel at the fitted ortho extent;
// tuned empirically against the helmet golden scene (ADR 0009).
constexpr float kShadowDepthBias = 0.0015f;

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
    math::float4x4 lightViewProj{1.0f};
    math::float4 shadowParams{0.0f};
    math::float4 timeParams{0.0f};
};
static_assert(sizeof(FrameUniformsData) == 1040);

struct PerDrawData {
    math::float4x4 model;
    math::float4x4 normalMatrix;
};
static_assert(sizeof(PerDrawData) == 128);

// std140 mirror of MaterialFactors in pbr.frag; uvTiling is the last
// engine-written member, custom shaders append their own members after it.
struct MaterialFactorsData {
    math::float4 baseColor{1.0f};
    math::float4 metallicRoughness{1.0f, 1.0f, 0.0f, 0.0f};
    math::float4 emissive{0.0f};
    math::float4 uvTiling{1.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(MaterialFactorsData) == 64);

MaterialFactorsData toFactors(const ForwardRenderer::MaterialParams& params) {
    return {
        .baseColor = {params.baseColor[0], params.baseColor[1], params.baseColor[2],
                      params.baseColor[3]},
        .metallicRoughness = {params.metallic, params.roughness, 0.0f, 0.0f},
        .emissive = {params.emissive[0], params.emissive[1], params.emissive[2], 0.0f},
        .uvTiling = {params.uvTiling[0], params.uvTiling[1], 0.0f, 0.0f},
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

math::Aabb mergeAabb(const math::Aabb& a, const math::Aabb& b) {
    return {
        .min = {std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
        .max = {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y),
                std::max(a.max.z, b.max.z)}};
}

// Declared byte size of the uniform_buffer at (set, binding); `fallback` when
// absent (e.g. a stage that does not reference it).
std::uint32_t bindingBufferSize(const shaderc::Reflection& reflection, std::uint32_t set,
                                std::uint32_t binding, std::uint32_t fallback) {
    for (const shaderc::ReflectionBinding& entry : reflection.bindings) {
        if (entry.set == set && entry.binding == binding) {
            return entry.bufferSize;
        }
    }
    return fallback;
}

// The pbr pipeline's fixed shape (vertex layout, targets, depth/cull/MSAA);
// shared by the pbr pipeline itself and every per-material custom pipeline
// (ADR 0011), which may only differ in fragment shader and set-1 layout.
gpu::RenderPipelineDesc pbrPipelineDesc(
    gpu::Ptr<gpu::ShaderModule> vertexShader, gpu::Ptr<gpu::ShaderModule> fragmentShader,
    std::vector<gpu::Ptr<gpu::BindGroupLayout>> bindGroupLayouts, std::uint32_t pushConstantSize) {
    return {
        .vertexShader = std::move(vertexShader),
        .fragmentShader = std::move(fragmentShader),
        .vertexBuffers =
            {{.stride = sizeof(asset::Vertex),
              .attributes =
                  {{.format = gpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
                   {.format = gpu::VertexFormat::Float32x3, .offset = 12, .shaderLocation = 1},
                   {.format = gpu::VertexFormat::Float32x4, .offset = 24, .shaderLocation = 2},
                   {.format = gpu::VertexFormat::Float32x2, .offset = 40, .shaderLocation = 3}}}},
        .bindGroupLayouts = std::move(bindGroupLayouts),
        .pushConstantSize = pushConstantSize,
        .colorFormats = {kHdrFormat},
        .depthStencil = {.format = kDepthFormat,
                         .depthWriteEnabled = true,
                         .depthCompare = gpu::CompareFunction::GreaterEqual},
        .cullMode = gpu::CullMode::Back,
        .sampleCount = kSampleCount,
    };
}

} // namespace

bool ForwardRenderer::init(gpu::Device& device, gpu::TextureFormat outputFormat) {
    device_ = &device;
    outputFormat_ = outputFormat;
    initTime_ = std::chrono::steady_clock::now();

    materialSampler_ = device.createSampler({.maxAnisotropy = 8});
    iblSampler_ = device.createSampler({
        .addressModeU = gpu::AddressMode::ClampToEdge,
        .addressModeV = gpu::AddressMode::ClampToEdge,
        .addressModeW = gpu::AddressMode::ClampToEdge,
    });
    tonemapSampler_ = device.createSampler({
        .addressModeU = gpu::AddressMode::ClampToEdge,
        .addressModeV = gpu::AddressMode::ClampToEdge,
        .addressModeW = gpu::AddressMode::ClampToEdge,
    });
    shadowSampler_ = device.createSampler({
        .addressModeU = gpu::AddressMode::ClampToEdge,
        .addressModeV = gpu::AddressMode::ClampToEdge,
        .addressModeW = gpu::AddressMode::ClampToEdge,
        .compare = gpu::CompareFunction::LessEqual,
    });
    // PCSS blocker search needs raw depth values, not a pass/fail compare.
    shadowRawSampler_ = device.createSampler({
        .addressModeU = gpu::AddressMode::ClampToEdge,
        .addressModeV = gpu::AddressMode::ClampToEdge,
        .addressModeW = gpu::AddressMode::ClampToEdge,
    });
    if (!materialSampler_ || !iblSampler_ || !tonemapSampler_ || !shadowSampler_ ||
        !shadowRawSampler_) {
        return false;
    }

    defaultWhite_ = makeSolidTexture(255, 255, 255, 255);
    defaultNormal_ = makeSolidTexture(128, 128, 255, 255);
    if (!defaultWhite_ || !defaultNormal_) {
        return false;
    }

    shadowMap_ = device.createTexture({
        .size = {kShadowMapSize, kShadowMapSize},
        .format = kDepthFormat,
        .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Sampled,
    });
    if (!shadowMap_) {
        return false;
    }

    if (!buildPipelines(true)) {
        return false;
    }

    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        frameUniforms_[slot] = device.createBuffer({
            .size = sizeof(FrameUniformsData),
            .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
        });
        if (!frameUniforms_[slot]) {
            return false;
        }
        frameGroups_[slot] = device.createBindGroup({
            .layout = frameLayout_,
            .entries = {{.binding = 0, .buffer = frameUniforms_[slot]},
                        {.binding = 1, .texture = shadowMap_},
                        {.binding = 2, .sampler = shadowSampler_},
                        {.binding = 3, .sampler = shadowRawSampler_}},
        });
        if (!frameGroups_[slot]) {
            return false;
        }
        shadowUniforms_[slot] = device.createBuffer({
            .size = sizeof(math::float4x4),
            .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
        });
        if (!shadowUniforms_[slot]) {
            return false;
        }
        shadowGroups_[slot] = device.createBindGroup({
            .layout = shadowLayout_,
            .entries = {{.binding = 0, .buffer = shadowUniforms_[slot]}},
        });
        if (!shadowGroups_[slot]) {
            return false;
        }
    }
    return true;
}

gpu::Ptr<gpu::Texture> ForwardRenderer::makeSolidTexture(std::uint8_t r, std::uint8_t g,
                                                         std::uint8_t b, std::uint8_t a) {
    KUMO_ASSERT(device_ != nullptr);
    gpu::Ptr<gpu::Texture> texture = device_->createTexture({
        .size = {1, 1},
        .format = gpu::TextureFormat::RGBA8Unorm,
        .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst,
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
        {.size = vertexBytes, .usage = gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst});
    out.indexBuffer = device_->createBuffer(
        {.size = indexBytes, .usage = gpu::BufferUsage::Index | gpu::BufferUsage::CopyDst});
    if (!out.vertexBuffer || !out.indexBuffer) {
        return false;
    }
    device_->queue().writeBuffer(*out.vertexBuffer, 0, mesh.vertices.data(), vertexBytes);
    device_->queue().writeBuffer(*out.indexBuffer, 0, mesh.indices.data(), indexBytes);
    out.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    out.localAabb = mesh.localAabb;
    return true;
}

bool ForwardRenderer::buildSharedMaterialSlots(std::size_t materialIndex) {
    KUMO_ASSERT(device_ != nullptr);
    KUMO_ASSERT(materialIndex < materialParams_.size());
    const MaterialFactorsData factors = toFactors(materialParams_[materialIndex]);
    const MaterialTextures& textures = materialTextures_[materialIndex];

    std::array<gpu::Ptr<gpu::Buffer>, kFrameSlots> buffers;
    std::array<gpu::Ptr<gpu::BindGroup>, kFrameSlots> groups;
    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        buffers[slot] = device_->createBuffer({
            .size = sizeof(MaterialFactorsData),
            .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
        });
        if (!buffers[slot]) {
            return false;
        }
        device_->queue().writeBuffer(*buffers[slot], 0, &factors, sizeof(factors));
        groups[slot] = device_->createBindGroup({
            .layout = materialLayout_,
            .entries = {{.binding = 0, .texture = textures.baseColor},
                        {.binding = 1, .texture = textures.metallicRoughness},
                        {.binding = 2, .texture = textures.normal},
                        {.binding = 3, .texture = textures.occlusion},
                        {.binding = 4, .texture = textures.emissive},
                        {.binding = 5, .sampler = materialSampler_},
                        {.binding = 6, .buffer = buffers[slot]}},
        });
        if (!groups[slot]) {
            return false;
        }
    }
    materialFactorBuffers_[materialIndex] = std::move(buffers);
    materialGroups_[materialIndex] = std::move(groups);
    materialDirty_[materialIndex] = {};
    return true;
}

bool ForwardRenderer::appendMaterial(const MaterialParams& params, const MaterialTextures& textures,
                                     const MaterialTextureIndices& indices) {
    KUMO_ASSERT(device_ != nullptr);
    materialFactorBuffers_.emplace_back();
    materialGroups_.emplace_back();
    materialDirty_.emplace_back();
    materialParams_.push_back(params);
    materialTextures_.push_back(textures);
    materialTextureIndices_.push_back(indices);
    materialShaders_.emplace_back(std::nullopt);
    const std::size_t index = materialParams_.size() - 1;
    if (!buildSharedMaterialSlots(index)) {
        materialFactorBuffers_.pop_back();
        materialGroups_.pop_back();
        materialDirty_.pop_back();
        materialParams_.pop_back();
        materialTextures_.pop_back();
        materialTextureIndices_.pop_back();
        materialShaders_.pop_back();
        return false;
    }
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

gpu::Ptr<gpu::Texture> ForwardRenderer::uploadTexture(gpu::CommandEncoder& encoder,
                                                      const asset::TextureData& tex) {
    KUMO_ASSERT(device_ != nullptr);
    if (tex.width == 0 || tex.height == 0 ||
        tex.rgba.size() < static_cast<std::size_t>(tex.width) * tex.height * 4) {
        return nullptr;
    }
    gpu::Ptr<gpu::Texture> texture = device_->createTexture({
        .size = {tex.width, tex.height},
        .format = tex.srgb ? gpu::TextureFormat::RGBA8UnormSrgb : gpu::TextureFormat::RGBA8Unorm,
        .usage =
            gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst,
        .mipLevelCount = fullMipChain(tex.width, tex.height),
    });
    if (!texture) {
        return nullptr;
    }
    device_->queue().writeTexture(*texture, tex.rgba.data(),
                                  static_cast<std::uint64_t>(tex.width) * 4,
                                  {tex.width, tex.height});
    encoder.generateMipmaps(*texture);
    return texture;
}

std::int32_t ForwardRenderer::addTexture(const asset::TextureData& texture) {
    KUMO_ASSERT(device_ != nullptr);
    gpu::Ptr<gpu::CommandEncoder> encoder = device_->queue().createCommandEncoder();
    if (!encoder) {
        return -1;
    }
    gpu::Ptr<gpu::Texture> uploaded = uploadTexture(*encoder, texture);
    if (!uploaded) {
        return -1;
    }
    encoder->finishAndSubmit();
    device_->queue().waitIdle();
    textures_.push_back(std::move(uploaded));
    return static_cast<std::int32_t>(textures_.size() - 1);
}

std::optional<ForwardRenderer::MaterialTextures>
ForwardRenderer::resolveMaterialTextures(const MaterialTextureIndices& indices) const {
    const auto resolve = [&](std::int32_t index,
                             const gpu::Ptr<gpu::Texture>& fallback) -> gpu::Ptr<gpu::Texture> {
        return index >= 0 && static_cast<std::size_t>(index) < textures_.size()
                   ? textures_[static_cast<std::size_t>(index)]
                   : fallback;
    };
    // Any explicit (>= 0) index that misses textures_ fails the whole material
    // instead of silently falling back, unlike -1 which means "use the default".
    const auto outOfRange = [&](std::int32_t index) {
        return index >= 0 && static_cast<std::size_t>(index) >= textures_.size();
    };
    if (outOfRange(indices.baseColor) || outOfRange(indices.metallicRoughness) ||
        outOfRange(indices.normal) || outOfRange(indices.occlusion) ||
        outOfRange(indices.emissive)) {
        return std::nullopt;
    }
    return MaterialTextures{
        .baseColor = resolve(indices.baseColor, defaultWhite_),
        .metallicRoughness = resolve(indices.metallicRoughness, defaultWhite_),
        .normal = resolve(indices.normal, defaultNormal_),
        .occlusion = resolve(indices.occlusion, defaultWhite_),
        .emissive = resolve(indices.emissive, defaultWhite_),
    };
}

std::int32_t ForwardRenderer::addMaterial(const MaterialParams& params) {
    KUMO_ASSERT(device_ != nullptr);
    if (!materialLayout_ || !defaultWhite_ || !defaultNormal_) {
        return -1;
    }
    if (!appendMaterial(params,
                        {.baseColor = defaultWhite_,
                         .metallicRoughness = defaultWhite_,
                         .normal = defaultNormal_,
                         .occlusion = defaultWhite_,
                         .emissive = defaultWhite_},
                        MaterialTextureIndices{})) {
        return -1;
    }
    return static_cast<std::int32_t>(materialGroups_.size() - 1);
}

std::int32_t ForwardRenderer::addMaterial(const MaterialParams& params,
                                          const MaterialTextureIndices& textures) {
    KUMO_ASSERT(device_ != nullptr);
    if (!materialLayout_ || !defaultWhite_ || !defaultNormal_) {
        return -1;
    }
    const std::optional<MaterialTextures> resolved = resolveMaterialTextures(textures);
    if (!resolved || !appendMaterial(params, *resolved, textures)) {
        return -1;
    }
    return static_cast<std::int32_t>(materialGroups_.size() - 1);
}

bool ForwardRenderer::rebuildCustomMaterialGroups(std::size_t materialIndex) {
    KUMO_ASSERT(device_ != nullptr);
    KUMO_ASSERT(materialIndex < materialShaders_.size() && materialShaders_[materialIndex]);
    const MaterialShaderRecord& record = *materialShaders_[materialIndex];
    const MaterialTextures& textures = materialTextures_[materialIndex];
    std::array<gpu::Ptr<gpu::BindGroup>, kFrameSlots> groups;
    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        groups[slot] = device_->createBindGroup({
            .layout = record.materialLayout,
            .entries = {{.binding = 0, .texture = textures.baseColor},
                        {.binding = 1, .texture = textures.metallicRoughness},
                        {.binding = 2, .texture = textures.normal},
                        {.binding = 3, .texture = textures.occlusion},
                        {.binding = 4, .texture = textures.emissive},
                        {.binding = 5, .sampler = materialSampler_},
                        {.binding = record.factorBufferBinding,
                         .buffer = materialFactorBuffers_[materialIndex][slot]}},
        });
        if (!groups[slot]) {
            return false;
        }
    }
    materialGroups_[materialIndex] = std::move(groups);
    return true;
}

bool ForwardRenderer::setMaterialTextures(std::uint32_t materialIndex,
                                          const MaterialTextureIndices& textures) {
    if (materialIndex >= materialParams_.size()) {
        return false;
    }
    const std::optional<MaterialTextures> resolved = resolveMaterialTextures(textures);
    if (!resolved) {
        return false;
    }
    // An in-flight frame may still reference the old bind groups (mirrors
    // setEnvironment's stance).
    device_->queue().waitIdle();
    const MaterialTextures previousTextures = materialTextures_[materialIndex];
    const MaterialTextureIndices previousIndices = materialTextureIndices_[materialIndex];
    materialTextures_[materialIndex] = *resolved;
    materialTextureIndices_[materialIndex] = textures;
    const bool rebuilt = materialShaders_[materialIndex]
                             ? rebuildCustomMaterialGroups(materialIndex)
                             : buildSharedMaterialSlots(materialIndex);
    if (!rebuilt) {
        // Restore the previous textures so a failed rebuild does not leave
        // materialTextures_ pointing at a bind group that was never built.
        materialTextures_[materialIndex] = previousTextures;
        materialTextureIndices_[materialIndex] = previousIndices;
        logError("setMaterialTextures: bind group rebuild failed for material {}", materialIndex);
        return false;
    }
    return true;
}

ForwardRenderer::MaterialTextureIndices
ForwardRenderer::materialTextureIndices(std::uint32_t index) const {
    return index < materialTextureIndices_.size() ? materialTextureIndices_[index]
                                                  : MaterialTextureIndices{};
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
    if (index >= materialParams_.size()) {
        return false;
    }
    materialParams_[index] = params;
    // Deferred to flushDirtyMaterials(): writing the slot the GPU may still be
    // reading this frame is exactly the tearing window double buffering avoids.
    for (bool& dirty : materialDirty_[index]) {
        dirty = true;
    }
    return true;
}

std::expected<void, std::vector<shaderc::CompileError>>
ForwardRenderer::setMaterialShader(std::uint32_t materialIndex, std::string_view fragmentSource) {
    if (materialIndex >= materialParams_.size()) {
        return std::unexpected(
            std::vector<shaderc::CompileError>{{.file = "",
                                                .line = 0,
                                                .message = "material index out of range",
                                                .secondStage = false}});
    }
    return compileMaterialShader(materialIndex, fragmentSource);
}

bool ForwardRenderer::clearMaterialShader(std::uint32_t materialIndex) {
    if (materialIndex >= materialShaders_.size() || !materialShaders_[materialIndex]) {
        return false;
    }
    if (!buildSharedMaterialSlots(materialIndex)) {
        return false;
    }
    materialShaders_[materialIndex] = std::nullopt;
    return true;
}

const std::string* ForwardRenderer::materialShaderSource(std::uint32_t materialIndex) const {
    if (materialIndex >= materialShaders_.size() || !materialShaders_[materialIndex]) {
        return nullptr;
    }
    return &materialShaders_[materialIndex]->fragmentSource;
}

bool ForwardRenderer::hasAnimatedMaterials() const {
    for (const std::optional<MaterialShaderRecord>& record : materialShaders_) {
        if (record.has_value() && record->referencesTime) {
            return true;
        }
    }
    return false;
}

std::expected<void, std::vector<shaderc::CompileError>>
ForwardRenderer::compileMaterialShader(std::size_t materialIndex, std::string_view source) {
    KUMO_ASSERT(device_ != nullptr);
    KUMO_ASSERT(materialIndex < materialParams_.size());

    const std::string sourceName = std::format("material{}.frag", materialIndex);
    auto compiled = shaderc::compileGlsl(
        source, shaderc::Stage::Fragment,
        {.sourceName = sourceName,
         .includeDirs = {(std::filesystem::path(KUMO_SHADER_DIR) / "include").string()}});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }

    // ADR 0029: sets 0/2 must stay reflection-identical to the shared pbr
    // fragment shader; only set 1 (the material's own factors) may differ.
    for (const std::uint32_t set : {0u, 2u}) {
        if (auto mismatch = detail::setMismatch(compiled->reflection, pbrFragReflection_, set)) {
            return std::unexpected(
                std::vector<shaderc::CompileError>{{.file = "",
                                                    .line = 0,
                                                    .message = "binding interface: " + *mismatch,
                                                    .secondStage = false}});
        }
    }

    // ADR 0025: 128B per-draw push constant budget, shared with the vertex stage.
    const std::uint32_t pushSize =
        std::max(pbrVertReflection_.pushConstantSize, compiled->reflection.pushConstantSize);
    if (pushSize > pbrPushConstantSize_) {
        return std::unexpected(std::vector<shaderc::CompileError>{
            {.file = "",
             .line = 0,
             .message = std::format("push constant size {} exceeds the shared pbr pipeline's {} "
                                    "byte budget",
                                    pushSize, pbrPushConstantSize_),
             .secondStage = false}});
    }

    const shaderc::ReflectionBinding* factors = nullptr;
    for (const shaderc::ReflectionBinding& binding : compiled->reflection.bindings) {
        if (binding.set == 1 && binding.type == "uniform_buffer") {
            factors = &binding;
            break;
        }
    }
    if (factors == nullptr) {
        return std::unexpected(std::vector<shaderc::CompileError>{
            {.file = "",
             .line = 0,
             .message = "binding interface: set 1 has no uniform buffer for material factors",
             .secondStage = false}});
    }

    gpu::Ptr<gpu::ShaderModule> fragmentModule = device_->createShaderModule({
        .stage = gpu::ShaderStage::Fragment,
        .mslSource = compiled->msl,
        .entryPoint = compiled->mslEntryPoint,
    });
    if (!fragmentModule) {
        return std::unexpected(
            std::vector<shaderc::CompileError>{{.file = "",
                                                .line = 0,
                                                .message = "fragment shader module creation failed",
                                                .secondStage = true}});
    }

    const detail::StageReflection layoutStages[] = {
        {&pbrVertReflection_, gpu::ShaderStage::Vertex},
        {&compiled->reflection, gpu::ShaderStage::Fragment}};
    std::vector<gpu::Ptr<gpu::BindGroupLayout>> layouts =
        detail::layoutsFromReflection(*device_, layoutStages);
    if (layouts.size() < 2 || !layouts[1]) {
        return std::unexpected(std::vector<shaderc::CompileError>{
            {.file = "",
             .line = 0,
             .message = "binding interface: failed to derive the set 1 layout",
             .secondStage = false}});
    }
    gpu::Ptr<gpu::BindGroupLayout> materialLayout = std::move(layouts[1]);

    gpu::Ptr<gpu::RenderPipeline> pipeline = device_->createRenderPipeline(pbrPipelineDesc(
        pbrVertModule_, fragmentModule, {frameLayout_, materialLayout, iblLayout_}, pushSize));
    if (!pipeline) {
        return std::unexpected(
            std::vector<shaderc::CompileError>{{.file = "",
                                                .line = 0,
                                                .message = "custom pipeline creation failed",
                                                .secondStage = true}});
    }

    // Buffer is sized from reflection exactly, so an older custom shader that
    // never picked up uvTiling gets a buffer matching its own declared size,
    // not one padded to the current CPU struct. Declared byte size alone
    // cannot decide how much of it the engine may write: std140 rounds a
    // block up in 16B steps, so a pre-uvTiling shader that appends its own
    // vec4 member after emissive reflects the same 64B bufferSize as the
    // current uvTiling layout. detail::factorPrefixSize checks the block's
    // member name instead, so the engine-written prefix never stomps a
    // shader's own member sitting right after emissive.
    const std::uint32_t bufferSize = factors->bufferSize;
    const std::uint32_t writeSize = detail::factorPrefixSize(*factors);
    const MaterialFactorsData factorsData = toFactors(materialParams_[materialIndex]);
    std::vector<std::byte> payload(bufferSize, std::byte{0});
    std::memcpy(payload.data(), &factorsData, writeSize);

    const MaterialTextures& textures = materialTextures_[materialIndex];
    std::array<gpu::Ptr<gpu::Buffer>, kFrameSlots> buffers;
    std::array<gpu::Ptr<gpu::BindGroup>, kFrameSlots> groups;
    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        buffers[slot] = device_->createBuffer({
            .size = bufferSize,
            .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
        });
        if (!buffers[slot]) {
            return std::unexpected(
                std::vector<shaderc::CompileError>{{.file = "",
                                                    .line = 0,
                                                    .message = "material buffer creation failed",
                                                    .secondStage = true}});
        }
        device_->queue().writeBuffer(*buffers[slot], 0, payload.data(), payload.size());
        groups[slot] = device_->createBindGroup({
            .layout = materialLayout,
            .entries = {{.binding = 0, .texture = textures.baseColor},
                        {.binding = 1, .texture = textures.metallicRoughness},
                        {.binding = 2, .texture = textures.normal},
                        {.binding = 3, .texture = textures.occlusion},
                        {.binding = 4, .texture = textures.emissive},
                        {.binding = 5, .sampler = materialSampler_},
                        {.binding = factors->binding, .buffer = buffers[slot]}},
        });
        if (!groups[slot]) {
            return std::unexpected(std::vector<shaderc::CompileError>{
                {.file = "",
                 .line = 0,
                 .message = "material bind group creation failed",
                 .secondStage = true}});
        }
    }

    materialFactorBuffers_[materialIndex] = std::move(buffers);
    materialGroups_[materialIndex] = std::move(groups);
    materialDirty_[materialIndex] = {};
    materialShaders_[materialIndex] = MaterialShaderRecord{
        .fragmentSource = std::string(source),
        .pipeline = std::move(pipeline),
        .materialLayout = std::move(materialLayout),
        .factorBufferBinding = factors->binding,
        .factorBufferSize = bufferSize,
        .factorWriteSize = writeSize,
        .referencesTime = detail::sourceReferencesTime(source),
    };
    return {};
}

bool ForwardRenderer::buildPipelines(bool deriveLayouts) {
    KUMO_ASSERT(device_ != nullptr);
    auto pbrVert = detail::loadStage(*device_, "pbr.vert", shaderc::Stage::Vertex);
    auto pbrFrag = detail::loadStage(*device_, "pbr.frag", shaderc::Stage::Fragment);
    auto skyboxVert = detail::loadStage(*device_, "skybox.vert", shaderc::Stage::Vertex);
    auto skyboxFrag = detail::loadStage(*device_, "skybox.frag", shaderc::Stage::Fragment);
    auto fullscreenVert = detail::loadStage(*device_, "fullscreen.vert", shaderc::Stage::Vertex);
    auto tonemapFrag = detail::loadStage(*device_, "tonemap.frag", shaderc::Stage::Fragment);
    auto shadowVert = detail::loadStage(*device_, "shadow.vert", shaderc::Stage::Vertex);
    auto shadowFrag = detail::loadStage(*device_, "shadow.frag", shaderc::Stage::Fragment);
    auto clayFrag = detail::loadStage(*device_, "debug_clay.frag", shaderc::Stage::Fragment);
    auto normalFrag = detail::loadStage(*device_, "debug_normal.frag", shaderc::Stage::Fragment);
    auto depthFrag = detail::loadStage(*device_, "debug_depth.frag", shaderc::Stage::Fragment);
    if (!pbrVert || !pbrFrag || !skyboxVert || !skyboxFrag || !fullscreenVert || !tonemapFrag ||
        !shadowVert || !shadowFrag || !clayFrag || !normalFrag || !depthFrag) {
        return false;
    }

    const detail::StageReflection pbrStages[] = {
        {&pbrVert->reflection, gpu::ShaderStage::Vertex},
        {&pbrFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection skyboxStages[] = {
        {&skyboxVert->reflection, gpu::ShaderStage::Vertex},
        {&skyboxFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection tonemapStages[] = {
        {&fullscreenVert->reflection, gpu::ShaderStage::Vertex},
        {&tonemapFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection shadowStages[] = {
        {&shadowVert->reflection, gpu::ShaderStage::Vertex},
        {&shadowFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection clayStages[] = {
        {&pbrVert->reflection, gpu::ShaderStage::Vertex},
        {&clayFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection normalStages[] = {
        {&pbrVert->reflection, gpu::ShaderStage::Vertex},
        {&normalFrag->reflection, gpu::ShaderStage::Fragment}};
    const detail::StageReflection depthStages[] = {
        {&pbrVert->reflection, gpu::ShaderStage::Vertex},
        {&depthFrag->reflection, gpu::ShaderStage::Fragment}};

    const std::string signature =
        detail::layoutSignature(pbrStages) + "|" + detail::layoutSignature(skyboxStages) + "|" +
        detail::layoutSignature(tonemapStages) + "|" + detail::layoutSignature(shadowStages) + "|" +
        detail::layoutSignature(clayStages) + "|" + detail::layoutSignature(normalStages) + "|" +
        detail::layoutSignature(depthStages);
    // A hot reload (deriveLayouts == false) that changes the binding table
    // rebuilds every dependent resource below instead of rejecting the reload
    // (ADR 0043): existing bind groups reference the layouts being replaced.
    const bool layoutChanged = !deriveLayouts && signature != layoutSignature_;
    if (deriveLayouts || layoutChanged) {
        auto pbrLayouts = detail::layoutsFromReflection(*device_, pbrStages);
        auto skyboxLayouts = detail::layoutsFromReflection(*device_, skyboxStages);
        auto tonemapLayouts = detail::layoutsFromReflection(*device_, tonemapStages);
        auto shadowLayouts = detail::layoutsFromReflection(*device_, shadowStages);
        if (pbrLayouts.size() < 3 || !pbrLayouts[0] || !pbrLayouts[1] || !pbrLayouts[2] ||
            skyboxLayouts.size() < 2 || !skyboxLayouts[1] || tonemapLayouts.size() < 2 ||
            !tonemapLayouts[1] || shadowLayouts.empty() || !shadowLayouts[0]) {
            logError("shader binding layout does not match the set 0/1/2 convention");
            return false;
        }
        frameLayout_ = pbrLayouts[0];
        materialLayout_ = pbrLayouts[1];
        iblLayout_ = pbrLayouts[2];
        skyboxLayout_ = skyboxLayouts[1];
        tonemapLayout_ = tonemapLayouts[1];
        shadowLayout_ = shadowLayouts[0];
        layoutSignature_ = signature;
    }

    const std::uint32_t pushSize =
        std::max(pbrVert->reflection.pushConstantSize, pbrFrag->reflection.pushConstantSize);
    gpu::Ptr<gpu::RenderPipeline> pbr = device_->createRenderPipeline(pbrPipelineDesc(
        pbrVert->module, pbrFrag->module, {frameLayout_, materialLayout_, iblLayout_}, pushSize));
    gpu::Ptr<gpu::RenderPipeline> skybox = device_->createRenderPipeline({
        .vertexShader = skyboxVert->module,
        .fragmentShader = skyboxFrag->module,
        .bindGroupLayouts = {frameLayout_, skyboxLayout_},
        .colorFormats = {kHdrFormat},
        .depthStencil = {.format = kDepthFormat,
                         .depthWriteEnabled = false,
                         .depthCompare = gpu::CompareFunction::GreaterEqual},
        .sampleCount = kSampleCount,
    });
    gpu::Ptr<gpu::RenderPipeline> tonemap = device_->createRenderPipeline({
        .vertexShader = fullscreenVert->module,
        .fragmentShader = tonemapFrag->module,
        .bindGroupLayouts = {nullptr, tonemapLayout_},
        .colorFormats = {outputFormat_},
    });
    gpu::Ptr<gpu::RenderPipeline> shadow = device_->createRenderPipeline({
        .vertexShader = shadowVert->module,
        .fragmentShader = shadowFrag->module,
        .vertexBuffers = {{.stride = sizeof(asset::Vertex),
                           .attributes = {{.format = gpu::VertexFormat::Float32x3,
                                           .offset = 0,
                                           .shaderLocation = 0}}}},
        .bindGroupLayouts = {shadowLayout_},
        .pushConstantSize = 64,
        .depthStencil = {.format = kDepthFormat,
                         .depthWriteEnabled = true,
                         .depthCompare = gpu::CompareFunction::Less,
                         .depthBias = 4.0f,
                         .depthBiasSlopeScale = 2.0f,
                         .depthBiasClamp = 0.0f},
        .cullMode = gpu::CullMode::Back,
        .sampleCount = 1,
    });
    // Built against the shared layouts, so the shared bind groups fit as-is.
    gpu::Ptr<gpu::RenderPipeline> debug[3];
    const gpu::Ptr<gpu::ShaderModule> debugFrags[3] = {clayFrag->module, normalFrag->module,
                                                       depthFrag->module};
    for (int i = 0; i < 3; ++i) {
        debug[i] = device_->createRenderPipeline(pbrPipelineDesc(
            pbrVert->module, debugFrags[i], {frameLayout_, materialLayout_, iblLayout_}, pushSize));
    }
    if (!pbr || !skybox || !tonemap || !shadow || !debug[0] || !debug[1] || !debug[2]) {
        return false;
    }
    pbrPipeline_ = std::move(pbr);
    skyboxPipeline_ = std::move(skybox);
    tonemapPipeline_ = std::move(tonemap);
    shadowPipeline_ = std::move(shadow);
    for (int i = 0; i < 3; ++i) {
        debugPipelines_[i] = std::move(debug[i]);
    }
    pbrVertModule_ = pbrVert->module;
    pbrVertReflection_ = pbrVert->reflection;
    pbrFragReflection_ = pbrFrag->reflection;
    pbrPushConstantSize_ = pushSize;

    if (layoutChanged) {
        rebuildMaterialResources();
        logInfo("shader reload rebuilt material resources");
    }
    return true;
}

void ForwardRenderer::rebuildMaterialResources() {
    KUMO_ASSERT(device_ != nullptr);

    // Frame uniform buffer size follows reflection (declared size can only grow
    // relative to the fixed CPU struct, which is written as a prefix each frame).
    const std::uint32_t frameBufferSize =
        std::max(bindingBufferSize(pbrVertReflection_, 0, 0,
                                   static_cast<std::uint32_t>(sizeof(FrameUniformsData))),
                 static_cast<std::uint32_t>(sizeof(FrameUniformsData)));
    for (std::uint32_t slot = 0; slot < kFrameSlots; ++slot) {
        if (!frameUniforms_[slot] || frameUniforms_[slot]->size() != frameBufferSize) {
            frameUniforms_[slot] = device_->createBuffer({
                .size = frameBufferSize,
                .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
            });
            if (!frameUniforms_[slot]) {
                logError("shader reload: frame uniform buffer rebuild failed (slot {})", slot);
                continue;
            }
        }
        // Always recreated: bind groups cache per-entry stage visibility at
        // creation, and the signature that got us here includes visibility.
        frameGroups_[slot] = device_->createBindGroup({
            .layout = frameLayout_,
            .entries = {{.binding = 0, .buffer = frameUniforms_[slot]},
                        {.binding = 1, .texture = shadowMap_},
                        {.binding = 2, .sampler = shadowSampler_},
                        {.binding = 3, .sampler = shadowRawSampler_}},
        });
        if (!frameGroups_[slot]) {
            logError("shader reload: frame bind group rebuild failed (slot {})", slot);
        }

        if (!shadowUniforms_[slot]) {
            shadowUniforms_[slot] = device_->createBuffer({
                .size = sizeof(math::float4x4),
                .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst,
            });
            if (!shadowUniforms_[slot]) {
                logError("shader reload: shadow uniform buffer rebuild failed (slot {})", slot);
                continue;
            }
        }
        shadowGroups_[slot] = device_->createBindGroup({
            .layout = shadowLayout_,
            .entries = {{.binding = 0, .buffer = shadowUniforms_[slot]}},
        });
        if (!shadowGroups_[slot]) {
            logError("shader reload: shadow bind group rebuild failed (slot {})", slot);
        }
    }

    for (std::size_t i = 0; i < materialParams_.size(); ++i) {
        bool rebuilt = false;
        if (materialShaders_[i]) {
            const std::string source = materialShaders_[i]->fragmentSource;
            if (auto result = compileMaterialShader(i, source); result.has_value()) {
                rebuilt = true;
            } else {
                for (const shaderc::CompileError& error : result.error()) {
                    logError("material {} custom shader incompatible after reload, reverting to "
                             "the shared pipeline: {}",
                             i, error.message);
                }
                materialShaders_[i] = std::nullopt;
            }
        }
        if (!rebuilt && !buildSharedMaterialSlots(i)) {
            logError("shader reload: material {} buffer/bind group rebuild failed", i);
        }
    }

    if (environment_.valid() && !buildEnvironmentGroups()) {
        logError("shader reload: ibl/skybox bind group rebuild failed");
    }
    if (hdrResolve_) {
        tonemapGroup_ = device_->createBindGroup({
            .layout = tonemapLayout_,
            .entries = {{.binding = 0, .texture = hdrResolve_},
                        {.binding = 1, .sampler = tonemapSampler_}},
        });
        if (!tonemapGroup_) {
            logError("shader reload: tonemap bind group rebuild failed");
        }
    }
}

bool ForwardRenderer::reloadPipelines() {
    return buildPipelines(false);
}

bool ForwardRenderer::buildEnvironmentGroups() {
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
    return iblGroup_ && skyboxGroup_;
}

bool ForwardRenderer::setEnvironment(const ibl::Environment& environment) {
    KUMO_ASSERT(device_ != nullptr);
    if (!environment.valid()) {
        logError("setEnvironment: invalid IBL environment");
        return false;
    }
    // The old environment's textures/bind groups may still be referenced by
    // an in-flight frame (mirrors resize()'s stance).
    device_->queue().waitIdle();
    // A failed build drops both groups, so restore the previous environment
    // rather than leave render() a half-built state mid-session.
    const ibl::Environment previous = environment_;
    environment_ = environment;
    if (!buildEnvironmentGroups()) {
        environment_ = previous;
        if (previous.valid() && buildEnvironmentGroups()) {
            logError("setEnvironment: bind group rebuild failed; previous environment restored");
        } else {
            logError("setEnvironment: bind group rebuild failed and the previous environment "
                     "could not be restored");
        }
        return false;
    }
    return true;
}

bool ForwardRenderer::loadScene(const asset::SceneAsset& sceneAsset,
                                const ibl::Environment& environment) {
    KUMO_ASSERT(device_ != nullptr);
    if (!environment.valid()) {
        logError("loadScene: invalid IBL environment");
        return false;
    }
    environment_ = environment;
    if (!buildEnvironmentGroups()) {
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

    gpu::Ptr<gpu::CommandEncoder> encoder = device_->queue().createCommandEncoder();

    textures_.clear();
    for (const asset::TextureData& tex : sceneAsset.textures) {
        gpu::Ptr<gpu::Texture> texture = uploadTexture(*encoder, tex);
        if (!texture) {
            return false;
        }
        textures_.push_back(std::move(texture));
    }

    auto textureOr = [&](std::int32_t index, const gpu::Ptr<gpu::Texture>& fallback) {
        return index >= 0 && static_cast<std::size_t>(index) < textures_.size()
                   ? textures_[static_cast<std::size_t>(index)]
                   : fallback;
    };
    // mat.*Texture fields are glTF-local indices into sceneAsset.textures,
    // which textures_ was just rebuilt from in the same order above, so they
    // double as renderer texture indices; out-of-range collapses to -1
    // (fallback), matching textureOr's own fallback rule.
    auto indexOr = [&](std::int32_t index) -> std::int32_t {
        return index >= 0 && static_cast<std::size_t>(index) < textures_.size() ? index : -1;
    };

    materialFactorBuffers_.clear();
    materialGroups_.clear();
    materialDirty_.clear();
    materialParams_.clear();
    materialTextures_.clear();
    materialTextureIndices_.clear();
    materialShaders_.clear();
    for (const asset::MaterialData& mat : sceneAsset.materials) {
        const MaterialTextures textures{.baseColor = textureOr(mat.baseColorTexture, defaultWhite_),
                                        .metallicRoughness =
                                            textureOr(mat.metallicRoughnessTexture, defaultWhite_),
                                        .normal = textureOr(mat.normalTexture, defaultNormal_),
                                        .occlusion = textureOr(mat.occlusionTexture, defaultWhite_),
                                        .emissive = textureOr(mat.emissiveTexture, defaultWhite_)};
        const MaterialTextureIndices indices{.baseColor = indexOr(mat.baseColorTexture),
                                             .metallicRoughness =
                                                 indexOr(mat.metallicRoughnessTexture),
                                             .normal = indexOr(mat.normalTexture),
                                             .occlusion = indexOr(mat.occlusionTexture),
                                             .emissive = indexOr(mat.emissiveTexture)};
        if (!appendMaterial(toParams(mat), textures, indices)) {
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

void ForwardRenderer::resize(gpu::Extent2D size) {
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
        .usage = gpu::TextureUsage::RenderTarget,
        .sampleCount = kSampleCount,
        .storageMode = gpu::StorageMode::TransientAttachment,
    });
    hdrResolve_ = device_->createTexture({
        .size = size,
        .format = kHdrFormat,
        .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Sampled,
    });
    depth_ = device_->createTexture({
        .size = size,
        .format = kDepthFormat,
        .usage = gpu::TextureUsage::RenderTarget,
        .sampleCount = kSampleCount,
        .storageMode = gpu::StorageMode::TransientAttachment,
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

void ForwardRenderer::setShadowsEnabled(bool enabled) {
    shadowsEnabled_ = enabled;
}

void ForwardRenderer::updateFrameUniforms(const scene::Scene& scene,
                                          const math::float4x4& lightViewProj,
                                          const math::float4& shadowParams) {
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
    data.lightViewProj = lightViewProj;
    data.shadowParams = shadowParams;
    const float secondsSinceInit =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - initTime_).count();
    // timeParams.y: the frame's max view-space depth, debug_depth.frag's span.
    float maxViewDepth = 0.0f;
    for (const DrawItem& drawItem : draws_) {
        const math::Aabb world =
            math::transformAabb(meshes_[drawItem.meshIndex].localAabb, drawItem.model);
        for (int corner = 0; corner < 8; ++corner) {
            const math::float4 p{(corner & 1) != 0 ? world.max.x : world.min.x,
                                 (corner & 2) != 0 ? world.max.y : world.min.y,
                                 (corner & 4) != 0 ? world.max.z : world.min.z, 1.0f};
            maxViewDepth = std::max(maxViewDepth, -(data.view * p).z);
        }
    }
    data.timeParams = {secondsSinceInit, maxViewDepth, 0.0f, 0.0f};

    device_->queue().writeBuffer(*frameUniforms_[frameSlot_], 0, &data, sizeof(data));
}

void ForwardRenderer::flushDirtyMaterials() {
    for (std::size_t i = 0; i < materialParams_.size(); ++i) {
        if (!materialDirty_[i][frameSlot_]) {
            continue;
        }
        const MaterialFactorsData factors = toFactors(materialParams_[i]);
        if (gpu::Ptr<gpu::Buffer>& buffer = materialFactorBuffers_[i][frameSlot_]) {
            // Shared-pipeline materials always write the full block (their
            // buffer is the template's); a custom shader's write size was
            // decided once at install time by detail::factorPrefixSize, from
            // its block's member names rather than its byte size (byte size
            // alone can't tell an appended member from uvTiling under std140
            // padding).
            const std::uint32_t writeSize = materialShaders_[i]
                                                ? materialShaders_[i]->factorWriteSize
                                                : static_cast<std::uint32_t>(sizeof(factors));
            device_->queue().writeBuffer(*buffer, 0, &factors, writeSize);
        }
        materialDirty_[i][frameSlot_] = false;
    }
}

void ForwardRenderer::render(gpu::CommandEncoder& encoder, const scene::Scene& scene,
                             gpu::Texture* output, const Overlay& overlay) {
    KUMO_ASSERT(device_ != nullptr);
    if (output == nullptr || !pbrPipeline_ || !hdrMsaa_ ||
        defaultMaterialIndex_ >= materialGroups_.size()) {
        return;
    }
    // Two uniform slots track the two frames in flight, so this write never
    // races the buffer the GPU is still reading.
    frameSlot_ = (frameSlot_ + 1) % kFrameSlots;
    flushDirtyMaterials();

    draws_.clear();
    math::Aabb sceneBounds{math::float3(std::numeric_limits<float>::max()),
                           math::float3(std::numeric_limits<float>::lowest())};
    scene.entities.forEach([&](scene::EntityId, const scene::Entity& entity) {
        if (entity.meshIndex < 0 || static_cast<std::size_t>(entity.meshIndex) >= meshes_.size()) {
            return;
        }
        const std::size_t materialIndex =
            entity.materialIndex >= 0 &&
                    static_cast<std::size_t>(entity.materialIndex) < materialGroups_.size()
                ? static_cast<std::size_t>(entity.materialIndex)
                : defaultMaterialIndex_;
        const math::float4x4 model = entity.transform.matrix();
        draws_.push_back({.meshIndex = static_cast<std::size_t>(entity.meshIndex),
                          .materialIndex = materialIndex,
                          .model = model});
        sceneBounds = mergeAabb(
            sceneBounds, math::transformAabb(
                             meshes_[static_cast<std::size_t>(entity.meshIndex)].localAabb, model));
    });
    // Shared-pipeline draws first, in their original relative order (a scene
    // with no custom materials never reorders, keeping the command stream and
    // golden output bit-identical); custom-pipeline draws then group by
    // material so setPipeline only switches on an actual pipeline change.
    const auto usesCustomPipeline = [&](const DrawItem& draw) {
        return materialShaders_[draw.materialIndex].has_value();
    };
    const auto customBegin =
        std::stable_partition(draws_.begin(), draws_.end(),
                              [&](const DrawItem& draw) { return !usesCustomPipeline(draw); });
    std::stable_sort(customBegin, draws_.end(), [](const DrawItem& a, const DrawItem& b) {
        return a.materialIndex < b.materialIndex;
    });

    // Shadow-casting light: the first directional light in the scene (ADR 0009;
    // CSM/multi-light shadows are a later extension). fitDirectionalShadow
    // returns identity for a degenerate direction/bounds, which also disables
    // the pass below.
    std::int32_t shadowLightIndex = -1;
    const std::span<const scene::Light> lights = scene.lights();
    for (std::size_t i = 0; i < lights.size(); ++i) {
        if (lights[i].type == scene::LightType::Directional) {
            shadowLightIndex = static_cast<std::int32_t>(i);
            break;
        }
    }
    math::float4x4 lightViewProj(1.0f);
    math::float4 shadowParams{0.0f, 0.0f, 0.0f, 0.0f};
    bool renderShadowPass = false;
    if (shadowsEnabled_ && shadowLightIndex >= 0 && !draws_.empty()) {
        const math::float4x4 fitted = detail::fitDirectionalShadow(
            sceneBounds, lights[static_cast<std::size_t>(shadowLightIndex)].direction);
        if (fitted != math::float4x4(1.0f)) {
            lightViewProj = fitted;
            shadowParams = {1.0f, 1.0f / static_cast<float>(kShadowMapSize), kShadowDepthBias,
                            static_cast<float>(shadowLightIndex)};
            renderShadowPass = true;
            device_->queue().writeBuffer(*shadowUniforms_[frameSlot_], 0, &lightViewProj,
                                         sizeof(lightViewProj));
        }
    }
    updateFrameUniforms(scene, lightViewProj, shadowParams);

    if (renderShadowPass) {
        gpu::RenderPassEncoder& shadowPass = encoder.beginRenderPass({
            .depthAttachment = {.texture = shadowMap_.get(),
                                .loadOp = gpu::LoadOp::Clear,
                                .storeOp = gpu::StoreOp::Store,
                                .clearDepth = 1.0f},
        });
        shadowPass.setPipeline(*shadowPipeline_);
        shadowPass.setBindGroup(0, *shadowGroups_[frameSlot_]);
        for (const DrawItem& drawItem : draws_) {
            const GpuMesh& mesh = meshes_[drawItem.meshIndex];
            shadowPass.setPushConstants(gpu::ShaderStage::Vertex, &drawItem.model,
                                        sizeof(drawItem.model));
            shadowPass.setVertexBuffer(0, *mesh.vertexBuffer);
            shadowPass.setIndexBuffer(*mesh.indexBuffer, gpu::IndexFormat::Uint32);
            shadowPass.drawIndexed(mesh.indexCount);
        }
        shadowPass.end();
    }

    gpu::RenderPassEncoder& scenePass = encoder.beginRenderPass({
        .colorAttachments = {{.texture = hdrMsaa_.get(),
                              .loadOp = gpu::LoadOp::Clear,
                              .storeOp = gpu::StoreOp::DontCare,
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .resolveTarget = hdrResolve_.get()}},
        .depthAttachment = {.texture = depth_.get(),
                            .loadOp = gpu::LoadOp::Clear,
                            .storeOp = gpu::StoreOp::DontCare,
                            .clearDepth = 0.0f},
    });
    // Null only after a doubly-failed environment swap (logged there); skip
    // the geometry rather than dereference it — the frame clears to black.
    if (iblGroup_) {
        // Debug views force one shared-layout pipeline for every draw; custom
        // materials' set-1 groups have a private layout, so those draws bind
        // the default material's group instead (the debug frags read none of it).
        gpu::RenderPipeline* debugPipeline =
            debugView_ != DebugView::None ? debugPipelines_[static_cast<int>(debugView_) - 1].get()
                                          : nullptr;
        scenePass.setPipeline(debugPipeline != nullptr ? *debugPipeline : *pbrPipeline_);
        scenePass.setBindGroup(0, *frameGroups_[frameSlot_]);
        scenePass.setBindGroup(2, *iblGroup_);
        gpu::RenderPipeline* currentPipeline =
            debugPipeline != nullptr ? debugPipeline : pbrPipeline_.get();
        for (const DrawItem& drawItem : draws_) {
            gpu::RenderPipeline* pipeline =
                debugPipeline != nullptr ? debugPipeline
                : usesCustomPipeline(drawItem)
                    ? materialShaders_[drawItem.materialIndex]->pipeline.get()
                    : pbrPipeline_.get();
            if (pipeline != currentPipeline) {
                // Metal's argument table state survives a pipeline switch, but
                // re-binding on switch is the safe contract across backends.
                scenePass.setPipeline(*pipeline);
                scenePass.setBindGroup(0, *frameGroups_[frameSlot_]);
                scenePass.setBindGroup(2, *iblGroup_);
                currentPipeline = pipeline;
            }
            const std::size_t groupIndex =
                debugPipeline != nullptr && materialShaders_[drawItem.materialIndex]
                    ? defaultMaterialIndex_
                    : drawItem.materialIndex;
            scenePass.setBindGroup(1, *materialGroups_[groupIndex][frameSlot_]);

            const GpuMesh& mesh = meshes_[drawItem.meshIndex];
            PerDrawData draw;
            draw.model = drawItem.model;
            draw.normalMatrix = math::transpose(math::inverse(draw.model));
            scenePass.setPushConstants(gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, &draw,
                                       sizeof(draw));
            scenePass.setVertexBuffer(0, *mesh.vertexBuffer);
            scenePass.setIndexBuffer(*mesh.indexBuffer, gpu::IndexFormat::Uint32);
            scenePass.drawIndexed(mesh.indexCount);
        }
    }
    if (skyboxGroup_) {
        scenePass.setPipeline(*skyboxPipeline_);
        scenePass.setBindGroup(0, *frameGroups_[frameSlot_]);
        scenePass.setBindGroup(1, *skyboxGroup_);
        scenePass.draw(3);
    }
    scenePass.end();

    gpu::RenderPassEncoder& tonemapPass = encoder.beginRenderPass({
        .colorAttachments = {{.texture = output,
                              .loadOp = gpu::LoadOp::Clear,
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
