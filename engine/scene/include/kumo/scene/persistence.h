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

struct SavedScene {
    std::string modelPath;
    std::vector<SavedEntity> entities;
    std::vector<Light> lights;
    Camera camera;
};

using MaterialLookup = std::function<std::optional<SavedMaterial>(std::int32_t materialIndex)>;

std::string saveSceneJson(const Scene& scene, std::string_view modelPath,
                          const MaterialLookup& materials);
std::expected<SavedScene, std::string> parseSceneJson(std::string_view json);

} // namespace kumo::scene
