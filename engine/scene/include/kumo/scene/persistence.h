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
};

struct SavedEntity {
    Entity entity;
    std::optional<SavedMaterial> material;
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
};

struct SavedScene {
    std::string modelPath;
    std::vector<SavedEntity> entities;
    std::vector<Light> lights;
    Camera camera;
    std::optional<SavedEnvironment> environment;
};

using MaterialLookup = std::function<std::optional<SavedMaterial>(std::int32_t materialIndex)>;

// `environment` is nullopt when the scene uses no procedural sky (e.g. the
// glTF-supplied HDR is still active); the key is then omitted entirely.
std::string saveSceneJson(const Scene& scene, std::string_view modelPath,
                          const MaterialLookup& materials,
                          const std::optional<SavedEnvironment>& environment = std::nullopt);
std::expected<SavedScene, std::string> parseSceneJson(std::string_view json);

} // namespace kumo::scene
