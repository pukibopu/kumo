#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <kumo/scene/scene.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::scene {

// The format is explicitly unstable (ADR 0016): loaders reject other versions.
inline constexpr int kSceneFormatVersion = 0;

// Renderer-free mirror of the material factors, so the scene layer never
// depends on the renderer.
struct SavedMaterial {
    float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3]{0.0f, 0.0f, 0.0f};
    // Absent key on read keeps this default (backward compatible with scenes
    // saved before M6.98).
    float uvTiling[2]{1.0f, 1.0f};
};

// Renderer-free mirror of a spliced surface parameter (MD); offsets are
// recomputed from order+type at load, never persisted.
struct SavedSurfaceParam {
    std::string name;
    bool isVec4 = false;
    float value[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

struct SavedEntity {
    Entity entity;
    std::optional<SavedMaterial> material;
    // Shader-assistant output (ADR 0011); plain source text so the scene layer
    // never depends on the renderer. Absent when the material uses the shared
    // pbr pipeline.
    std::optional<std::string> shaderSource;
    // Surface parameters riding on shaderSource (MD); empty when none.
    std::vector<SavedSurfaceParam> surfaceParams;
};

// Renderer/asset-free mirror of asset::ProceduralSkyDesc (scene must not
// depend on asset): plain floats, same field set, converted at the facade
// layer where both types are visible.
struct SavedEnvironment {
    float zenithColor[3]{0.18f, 0.32f, 0.62f};
    float horizonColor[3]{0.72f, 0.78f, 0.85f};
    float groundColor[3]{0.22f, 0.20f, 0.18f};
    float sunDirection[3]{-0.4f, -0.7f, -0.5f};
    float sunColor[3]{1.0f, 0.95f, 0.85f};
    float sunIntensity = 60.0f;
    float sunAngularRadiusDeg = 1.5f;
    float exposure = 1.0f;
    // Absent (nullopt) means procedural sky, using the fields above; present
    // names an HDR file under <assetDir>/env/, and the procedural fields are
    // then ignored (but still written, so the file stays human-editable).
    std::optional<std::string> file;
};

struct SavedScene {
    std::string modelPath;
    std::vector<SavedEntity> entities;
    std::vector<Light> lights;
    Camera camera;
    std::optional<SavedEnvironment> environment;
};

using MaterialLookup = std::function<std::optional<SavedMaterial>(std::int32_t materialIndex)>;
// Renderer-side custom fragment shader for a material index, nullopt when
// uncustomized (mirrors ForwardRenderer::materialShaderSource).
using ShaderLookup = std::function<std::optional<std::string>(std::int32_t materialIndex)>;
// Surface parameters for a material index, empty when none (MD).
using SurfaceLookup = std::function<std::vector<SavedSurfaceParam>(std::int32_t materialIndex)>;

// `environment` is nullopt when the scene uses no procedural sky (e.g. the
// glTF-supplied HDR is still active); the key is then omitted entirely.
// `shaders`/`surfaces` are defaulted-empty for callers that never persist
// custom shaders.
std::string saveSceneJson(const Scene& scene, std::string_view modelPath,
                          const MaterialLookup& materials,
                          const std::optional<SavedEnvironment>& environment = std::nullopt,
                          const ShaderLookup& shaders = {}, const SurfaceLookup& surfaces = {});
std::expected<SavedScene, std::string> parseSceneJson(std::string_view json);

} // namespace kumo::scene
