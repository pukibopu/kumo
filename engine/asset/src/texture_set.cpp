#include <kumo/asset/texture_set.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace kumo::asset {

namespace {

std::optional<std::filesystem::path> probeFile(const std::filesystem::path& dir, const char* stem) {
    for (const char* ext : {".png", ".jpg"}) {
        std::filesystem::path candidate = dir / (std::string(stem) + ext);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::expected<std::optional<TextureData>, std::string>
loadOptional(const std::filesystem::path& dir, const char* stem, bool srgb) {
    const std::optional<std::filesystem::path> path = probeFile(dir, stem);
    if (!path.has_value()) {
        return std::optional<TextureData>(std::nullopt);
    }
    auto image = loadImage(*path);
    if (!image.has_value()) {
        return std::unexpected(image.error());
    }
    image->srgb = srgb;
    return std::make_optional(std::move(*image));
}

} // namespace

std::expected<TextureSetData, std::string> loadTextureSet(const std::filesystem::path& dir) {
    TextureSetData out;

    auto albedo = loadOptional(dir, "albedo", /*srgb=*/true);
    if (!albedo.has_value()) {
        return std::unexpected(albedo.error());
    }
    if (!albedo->has_value()) {
        return std::unexpected(
            std::format("texture set at {} is missing albedo.png/.jpg", dir.string()));
    }
    out.baseColor = std::move(**albedo);

    auto normal = loadOptional(dir, "normal", /*srgb=*/false);
    if (!normal.has_value()) {
        return std::unexpected(normal.error());
    }
    out.normal = std::move(*normal);

    auto occlusion = loadOptional(dir, "ao", /*srgb=*/false);
    if (!occlusion.has_value()) {
        return std::unexpected(occlusion.error());
    }
    out.occlusion = std::move(*occlusion);

    auto roughness = loadOptional(dir, "roughness", /*srgb=*/false);
    if (!roughness.has_value()) {
        return std::unexpected(roughness.error());
    }
    auto metalness = loadOptional(dir, "metalness", /*srgb=*/false);
    if (!metalness.has_value()) {
        return std::unexpected(metalness.error());
    }

    if (roughness->has_value() || metalness->has_value()) {
        if (roughness->has_value() && metalness->has_value() &&
            ((*roughness)->width != (*metalness)->width ||
             (*roughness)->height != (*metalness)->height)) {
            return std::unexpected(std::format(
                "texture set at {}: roughness ({}x{}) and metalness ({}x{}) size mismatch",
                dir.string(), (*roughness)->width, (*roughness)->height, (*metalness)->width,
                (*metalness)->height));
        }
        const std::uint32_t width =
            roughness->has_value() ? (*roughness)->width : (*metalness)->width;
        const std::uint32_t height =
            roughness->has_value() ? (*roughness)->height : (*metalness)->height;

        TextureData packed;
        packed.width = width;
        packed.height = height;
        packed.srgb = false;
        packed.rgba.assign(static_cast<std::size_t>(width) * height * 4, std::uint8_t{0});
        const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < pixelCount; ++i) {
            // Source images are decoded RGBA8 grayscale (R==G==B); read R as
            // the scalar value. Missing metalness reads as 0 (glTF packing).
            packed.rgba[i * 4 + 1] = roughness->has_value() ? (*roughness)->rgba[i * 4] : 255;
            packed.rgba[i * 4 + 2] = metalness->has_value() ? (*metalness)->rgba[i * 4] : 0;
            packed.rgba[i * 4 + 3] = 255;
        }
        out.metallicRoughness = std::move(packed);
    }

    return out;
}

} // namespace kumo::asset
