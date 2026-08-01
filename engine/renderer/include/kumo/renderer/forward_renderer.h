#pragma once

#include <kumo/asset/asset.h>
#include <kumo/gpu/gpu.h>
#include <kumo/renderer/ibl.h>
#include <kumo/scene/scene.h>
#include <kumo/shaderc/compiler.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::renderer {

// Forward PBR renderer: MSAA 4x HDR pass (meshes + skybox) resolved to a single
// sample target, then an ACES tonemap pass onto the caller's output texture.
// Bind group layouts come from shader reflection (ADR 0040).
class ForwardRenderer {
public:
    using Overlay = std::function<void(gpu::RenderPassEncoder&)>;

    // Mirrors the MaterialFactors uniform block in pbr.frag.
    struct MaterialParams {
        float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
        float metallic = 1.0f;
        float roughness = 1.0f;
        float emissive[3]{0.0f, 0.0f, 0.0f};
    };

    bool init(gpu::Device& device, gpu::TextureFormat outputFormat);
    bool loadScene(const asset::SceneAsset& sceneAsset, const ibl::Environment& environment);
    // Swaps the IBL/skybox environment on an already-loaded scene; rejects an
    // invalid environment and leaves the current one untouched. Waits for the
    // GPU like resize() does: the old environment's textures/bind groups may
    // still be referenced by an in-flight frame.
    bool setEnvironment(const ibl::Environment& environment);
    void resize(gpu::Extent2D size);

    // Incremental uploads on top of a loaded scene; indices stay valid until the
    // next loadScene. All return -1 / false instead of asserting on bad input.
    std::int32_t addMesh(const asset::MeshData& mesh);
    // Untextured: every texture slot binds the built-in fallback.
    std::int32_t addMaterial(const MaterialParams& params);
    std::uint32_t meshCount() const;
    // Local-space bounds recorded at upload; null when `index` is out of range.
    const math::Aabb* meshLocalAabb(std::uint32_t index) const;
    std::uint32_t materialCount() const;
    std::uint32_t defaultMaterialIndex() const;
    const MaterialParams* materialParams(std::uint32_t index) const;
    bool setMaterialParams(std::uint32_t index, const MaterialParams& params);

    // Compiles and installs a material-private fragment shader over the shared
    // pbr vertex stage (ADR 0011). Sets 0 and 2 must stay reflection-identical
    // to the shared pbr pipeline; set 1 may differ, and the material's layout,
    // factor buffers and bind groups are rebuilt from the new reflection. On
    // any error the material keeps its current pipeline untouched.
    std::expected<void, std::vector<shaderc::CompileError>>
    setMaterialShader(std::uint32_t materialIndex, std::string_view fragmentSource);
    // Reverts the material to the shared pipeline. False when index is invalid
    // or the material has no custom shader.
    bool clearMaterialShader(std::uint32_t materialIndex);
    // Null when the material has no custom shader.
    const std::string* materialShaderSource(std::uint32_t materialIndex) const;

    // Multipliers on top of the material metallic/roughness factors.
    void setMaterialOverride(float metallic, float roughness);
    // Directional-light PCF shadow mapping (ADR 0009); disabling clears shadowParams
    // so the frame stops sampling the shadow map, leaving its stale contents unused.
    void setShadowsEnabled(bool enabled);
    bool shadowsEnabled() const { return shadowsEnabled_; }
    // Recompiles every shader and swaps the pipelines; keeps the current ones on
    // any error, so a broken edit never interrupts rendering.
    bool reloadPipelines();
    // Records both passes; `overlay` is invoked inside the tonemap pass (ImGui).
    void render(gpu::CommandEncoder& encoder, const scene::Scene& scene, gpu::Texture* output,
                const Overlay& overlay = {});

private:
    struct GpuMesh {
        gpu::Ptr<gpu::Buffer> vertexBuffer;
        gpu::Ptr<gpu::Buffer> indexBuffer;
        std::uint32_t indexCount = 0;
        math::Aabb localAabb;
    };

    struct MaterialTextures {
        gpu::Ptr<gpu::Texture> baseColor;
        gpu::Ptr<gpu::Texture> metallicRoughness;
        gpu::Ptr<gpu::Texture> normal;
        gpu::Ptr<gpu::Texture> occlusion;
        gpu::Ptr<gpu::Texture> emissive;
    };

    // Per-material fragment shader override (ADR 0011); the set-1 layout and
    // factor buffer size are re-derived from this shader's own reflection.
    struct MaterialShaderRecord {
        std::string fragmentSource;
        gpu::Ptr<gpu::RenderPipeline> pipeline;
        gpu::Ptr<gpu::BindGroupLayout> materialLayout;
        std::uint32_t factorBufferBinding = 0;
        std::uint32_t factorBufferSize = 0;
    };

    struct DrawItem {
        std::size_t meshIndex = 0;
        std::size_t materialIndex = 0;
        math::float4x4 model;
    };

    bool buildPipelines(bool deriveLayouts);
    // Recreates frame uniform buffers/groups, every material's factor buffers
    // and bind groups, and the ibl/skybox/tonemap groups after a layout change.
    void rebuildMaterialResources();
    // Builds iblGroup_/skyboxGroup_ from the current environment_; shared by
    // loadScene, rebuildMaterialResources and setEnvironment so all three
    // construct the same bind groups from the same layout.
    bool buildEnvironmentGroups();
    void updateFrameUniforms(const scene::Scene& scene, const math::float4x4& lightViewProj,
                             const math::float4& shadowParams);
    // Writes the current frameSlot_ buffer for every material marked dirty in
    // that slot; called once per render() before recording the scene pass.
    void flushDirtyMaterials();
    gpu::Ptr<gpu::Texture> makeSolidTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                            std::uint8_t a);
    bool uploadMesh(const asset::MeshData& mesh, GpuMesh& out);
    bool appendMaterial(const MaterialParams& params, const MaterialTextures& textures);
    // Builds (or rebuilds) the shared-layout 48B factor buffers and bind groups
    // for one material slot, across every frame slot.
    bool buildSharedMaterialSlots(std::size_t materialIndex);
    // Compiles `source` as this material's fragment shader and, on success,
    // rebuilds its pipeline/layout/buffers/groups in place. Leaves everything
    // untouched on failure.
    std::expected<void, std::vector<shaderc::CompileError>>
    compileMaterialShader(std::size_t materialIndex, std::string_view source);

    gpu::Device* device_ = nullptr;
    gpu::TextureFormat outputFormat_ = gpu::TextureFormat::BGRA8Unorm;
    gpu::Extent2D size_;

    gpu::Ptr<gpu::BindGroupLayout> frameLayout_;
    gpu::Ptr<gpu::BindGroupLayout> materialLayout_;
    gpu::Ptr<gpu::BindGroupLayout> iblLayout_;
    gpu::Ptr<gpu::BindGroupLayout> skyboxLayout_;
    gpu::Ptr<gpu::BindGroupLayout> tonemapLayout_;
    gpu::Ptr<gpu::BindGroupLayout> shadowLayout_;
    // Bind groups outlive reloads, so a reload must reproduce this signature.
    std::string layoutSignature_;

    gpu::Ptr<gpu::RenderPipeline> pbrPipeline_;
    gpu::Ptr<gpu::RenderPipeline> skyboxPipeline_;
    gpu::Ptr<gpu::RenderPipeline> tonemapPipeline_;
    gpu::Ptr<gpu::RenderPipeline> shadowPipeline_;

    // Shared pbr vertex stage + its reflection, reused when building a custom
    // material pipeline (ADR 0011); the shared fragment reflection is the
    // compatibility reference for sets 0/2 (ADR 0029).
    gpu::Ptr<gpu::ShaderModule> pbrVertModule_;
    shaderc::Reflection pbrVertReflection_;
    shaderc::Reflection pbrFragReflection_;
    std::uint32_t pbrPushConstantSize_ = 0;

    gpu::Ptr<gpu::Texture> hdrMsaa_;
    gpu::Ptr<gpu::Texture> hdrResolve_;
    gpu::Ptr<gpu::Texture> depth_;
    gpu::Ptr<gpu::BindGroup> tonemapGroup_;

    // Fixed size, allocated once in init(); resize() never touches it.
    gpu::Ptr<gpu::Texture> shadowMap_;

    static constexpr std::uint32_t kFrameSlots = 2;
    gpu::Ptr<gpu::Buffer> frameUniforms_[kFrameSlots];
    gpu::Ptr<gpu::BindGroup> frameGroups_[kFrameSlots];
    gpu::Ptr<gpu::Buffer> shadowUniforms_[kFrameSlots];
    gpu::Ptr<gpu::BindGroup> shadowGroups_[kFrameSlots];
    std::uint32_t frameSlot_ = 0;
    bool shadowsEnabled_ = true;

    gpu::Ptr<gpu::Sampler> materialSampler_;
    gpu::Ptr<gpu::Sampler> iblSampler_;
    gpu::Ptr<gpu::Sampler> tonemapSampler_;
    gpu::Ptr<gpu::Sampler> shadowSampler_;
    // Non-comparison sampler over the same shadow map, for PCSS blocker search
    // (raw depth reads; shadowSampler_ only does hardware compare).
    gpu::Ptr<gpu::Sampler> shadowRawSampler_;

    ibl::Environment environment_;
    gpu::Ptr<gpu::BindGroup> iblGroup_;
    gpu::Ptr<gpu::BindGroup> skyboxGroup_;

    gpu::Ptr<gpu::Texture> defaultWhite_;
    gpu::Ptr<gpu::Texture> defaultNormal_;

    std::vector<GpuMesh> meshes_;
    std::vector<gpu::Ptr<gpu::Texture>> textures_;
    // factor buffers/groups are double-buffered like frameUniforms_/frameGroups_
    // so setMaterialParams never overwrites a slot the GPU may still be reading.
    std::vector<std::array<gpu::Ptr<gpu::Buffer>, kFrameSlots>> materialFactorBuffers_;
    std::vector<std::array<gpu::Ptr<gpu::BindGroup>, kFrameSlots>> materialGroups_;
    std::vector<std::array<bool, kFrameSlots>> materialDirty_;
    std::vector<MaterialParams> materialParams_;
    // Original textures a material was created with; rebuilds (custom shader
    // install/clear, hot-reload layout change) recreate bind groups from these.
    std::vector<MaterialTextures> materialTextures_;
    // nullopt when the material uses the shared pbr pipeline.
    std::vector<std::optional<MaterialShaderRecord>> materialShaders_;
    // Sentinel material for entities without one; recorded explicitly so that
    // appending materials cannot displace it.
    std::size_t defaultMaterialIndex_ = 0;

    // Reusable per-frame scratch: cleared and refilled each render() call to
    // avoid a per-frame heap allocation.
    std::vector<DrawItem> draws_;

    float overrideMetallic_ = 1.0f;
    float overrideRoughness_ = 1.0f;
};

} // namespace kumo::renderer
