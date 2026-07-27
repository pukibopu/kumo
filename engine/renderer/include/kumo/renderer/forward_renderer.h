#pragma once

#include <kumo/asset/asset.h>
#include <kumo/renderer/ibl.h>
#include <kumo/rhi/rhi.h>
#include <kumo/scene/scene.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kumo::renderer {

// Forward PBR renderer: MSAA 4x HDR pass (meshes + skybox) resolved to a single
// sample target, then an ACES tonemap pass onto the caller's output texture.
// Bind group layouts come from shader reflection (ADR 0040).
class ForwardRenderer {
public:
    using Overlay = std::function<void(rhi::RenderPassEncoder&)>;

    // Mirrors the MaterialFactors uniform block in pbr.frag.
    struct MaterialParams {
        float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
        float metallic = 1.0f;
        float roughness = 1.0f;
        float emissive[3]{0.0f, 0.0f, 0.0f};
    };

    bool init(rhi::Device& device, rhi::TextureFormat outputFormat);
    bool loadScene(const asset::SceneAsset& sceneAsset, const ibl::Environment& environment);
    void resize(rhi::Extent2D size);

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

    // Multipliers on top of the material metallic/roughness factors.
    void setMaterialOverride(float metallic, float roughness);
    // Recompiles every shader and swaps the pipelines; keeps the current ones on
    // any error, so a broken edit never interrupts rendering.
    bool reloadPipelines();
    // Records both passes; `overlay` is invoked inside the tonemap pass (ImGui).
    void render(rhi::CommandEncoder& encoder, const scene::Scene& scene, rhi::Texture* output,
                const Overlay& overlay = {});

private:
    struct GpuMesh {
        rhi::Ptr<rhi::Buffer> vertexBuffer;
        rhi::Ptr<rhi::Buffer> indexBuffer;
        std::uint32_t indexCount = 0;
        math::Aabb localAabb;
    };

    struct MaterialTextures {
        rhi::Ptr<rhi::Texture> baseColor;
        rhi::Ptr<rhi::Texture> metallicRoughness;
        rhi::Ptr<rhi::Texture> normal;
        rhi::Ptr<rhi::Texture> occlusion;
        rhi::Ptr<rhi::Texture> emissive;
    };

    bool buildPipelines(bool deriveLayouts);
    void updateFrameUniforms(const scene::Scene& scene);
    rhi::Ptr<rhi::Texture> makeSolidTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                            std::uint8_t a);
    bool uploadMesh(const asset::MeshData& mesh, GpuMesh& out);
    bool appendMaterial(const MaterialParams& params, const MaterialTextures& textures);

    rhi::Device* device_ = nullptr;
    rhi::TextureFormat outputFormat_ = rhi::TextureFormat::BGRA8Unorm;
    rhi::Extent2D size_;

    rhi::Ptr<rhi::BindGroupLayout> frameLayout_;
    rhi::Ptr<rhi::BindGroupLayout> materialLayout_;
    rhi::Ptr<rhi::BindGroupLayout> iblLayout_;
    rhi::Ptr<rhi::BindGroupLayout> skyboxLayout_;
    rhi::Ptr<rhi::BindGroupLayout> tonemapLayout_;
    // Bind groups outlive reloads, so a reload must reproduce this signature.
    std::string layoutSignature_;

    rhi::Ptr<rhi::RenderPipeline> pbrPipeline_;
    rhi::Ptr<rhi::RenderPipeline> skyboxPipeline_;
    rhi::Ptr<rhi::RenderPipeline> tonemapPipeline_;

    rhi::Ptr<rhi::Texture> hdrMsaa_;
    rhi::Ptr<rhi::Texture> hdrResolve_;
    rhi::Ptr<rhi::Texture> depth_;
    rhi::Ptr<rhi::BindGroup> tonemapGroup_;

    static constexpr std::uint32_t kFrameSlots = 2;
    rhi::Ptr<rhi::Buffer> frameUniforms_[kFrameSlots];
    rhi::Ptr<rhi::BindGroup> frameGroups_[kFrameSlots];
    std::uint32_t frameSlot_ = 0;

    rhi::Ptr<rhi::Sampler> materialSampler_;
    rhi::Ptr<rhi::Sampler> iblSampler_;
    rhi::Ptr<rhi::Sampler> tonemapSampler_;

    ibl::Environment environment_;
    rhi::Ptr<rhi::BindGroup> iblGroup_;
    rhi::Ptr<rhi::BindGroup> skyboxGroup_;

    rhi::Ptr<rhi::Texture> defaultWhite_;
    rhi::Ptr<rhi::Texture> defaultNormal_;

    std::vector<GpuMesh> meshes_;
    std::vector<rhi::Ptr<rhi::Texture>> textures_;
    std::vector<rhi::Ptr<rhi::Buffer>> materialFactorBuffers_;
    std::vector<rhi::Ptr<rhi::BindGroup>> materialGroups_;
    std::vector<MaterialParams> materialParams_;
    // Sentinel material for entities without one; recorded explicitly so that
    // appending materials cannot displace it.
    std::size_t defaultMaterialIndex_ = 0;

    float overrideMetallic_ = 1.0f;
    float overrideRoughness_ = 1.0f;
};

} // namespace kumo::renderer
