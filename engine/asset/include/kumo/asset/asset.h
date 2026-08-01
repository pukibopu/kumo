#pragma once

#include <kumo/math/math.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace kumo::asset {

// Interleaved vertex: position, normal, tangent (xyzw, w = handedness), uv.
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v;
};

// GPU vertex buffers upload this struct verbatim; the layout must stay fixed.
static_assert(sizeof(Vertex) == 48);
static_assert(offsetof(Vertex, nx) == 12);
static_assert(offsetof(Vertex, tx) == 24);
static_assert(offsetof(Vertex, u) == 40);

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::int32_t materialIndex = -1;
    // Bounds in the mesh's own space, before any node transform.
    math::Aabb localAabb;
};

// Decoded texel data is always RGBA8; `srgb` records the intended color space.
struct TextureData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool srgb = false;
    std::vector<std::uint8_t> rgba;
};

struct MaterialData {
    float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3]{0.0f, 0.0f, 0.0f};
    std::int32_t baseColorTexture = -1;
    std::int32_t metallicRoughnessTexture = -1;
    std::int32_t normalTexture = -1;
    std::int32_t occlusionTexture = -1;
    std::int32_t emissiveTexture = -1;
};

// One node instance references exactly one mesh (glTF primitive), pre-flattened
// to world space (ADR 0010, static scene subset).
struct NodeInstance {
    std::string name;
    math::float4x4 worldTransform{1.0f};
    std::int32_t meshIndex = -1;
};

struct SceneAsset {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<TextureData> textures;
    std::vector<NodeInstance> nodes;
};

std::expected<SceneAsset, std::string> loadGltf(const std::filesystem::path& path);

// Equirectangular HDR environment, RGBA32F. V is kept unflipped; equirect
// sampling accounts for orientation in the shader.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;
};

std::expected<HdrImage, std::string> loadHdr(const std::filesystem::path& path);

// RGBA8 image IO used by screenshots and golden-image tests.
std::expected<TextureData, std::string> loadImage(const std::filesystem::path& path);
bool writePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
              const std::uint8_t* rgba);

struct DownscaledImage {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Aspect-preserving sRGB 8-bit downscale for screenshot attachments (the vision
// feedback loop, M6.97). A no-op copy when both dimensions already fit within
// maxLongSide; never upscales.
DownscaledImage downscaleRgba(const std::uint8_t* src, std::uint32_t width, std::uint32_t height,
                              std::uint32_t maxLongSide);

} // namespace kumo::asset
