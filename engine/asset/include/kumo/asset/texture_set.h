#pragma once

#include <kumo/asset/asset.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace kumo::asset {

// One PBR texture set loaded from a directory convention (M6.98 PR-1):
// baseColor is sRGB, everything else is linear. metallicRoughness follows the
// glTF packing (G channel roughness, B channel metallic).
struct TextureSetData {
    std::optional<TextureData> baseColor;
    std::optional<TextureData> metallicRoughness;
    std::optional<TextureData> normal;
    std::optional<TextureData> occlusion;
};

// Loads `dir/{albedo,normal,roughness,metalness,ao}.{png,jpg}` (both
// extensions are probed; png wins when both exist). albedo is required,
// everything else is optional. When present, roughness and metalness are
// packed CPU-side into one metallicRoughness image (G = roughness, B =
// metallic; a missing metalness reads as 0); their dimensions must match when
// both are present, otherwise loading fails.
std::expected<TextureSetData, std::string> loadTextureSet(const std::filesystem::path& dir);

} // namespace kumo::asset
