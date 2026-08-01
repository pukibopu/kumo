#include <doctest/doctest.h>

#include <kumo/asset/asset.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace kumo;

TEST_CASE("loadGltf loads DamagedHelmet.glb") {
    std::filesystem::path path =
        std::filesystem::path(KUMO_ASSET_DIR) / "models" / "DamagedHelmet.glb";
    auto result = asset::loadGltf(path);
    REQUIRE(result.has_value());
    const asset::SceneAsset& s = *result;

    CHECK(s.nodes.size() == 1);
    CHECK(s.meshes.size() >= 1);

    const asset::NodeInstance& node = s.nodes.front();
    REQUIRE(node.meshIndex >= 0);
    REQUIRE(static_cast<std::size_t>(node.meshIndex) < s.meshes.size());

    const asset::MeshData& mesh = s.meshes[static_cast<std::size_t>(node.meshIndex)];
    CHECK(mesh.vertices.size() > 1000);
    CHECK(mesh.indices.size() > 0);
    CHECK(mesh.indices.size() % 3 == 0);

    CHECK(mesh.localAabb.min.x < mesh.localAabb.max.x);
    CHECK(mesh.localAabb.min.y < mesh.localAabb.max.y);
    CHECK(mesh.localAabb.min.z < mesh.localAabb.max.z);
    bool insideAabb = true;
    for (const asset::Vertex& v : mesh.vertices) {
        if (v.px < mesh.localAabb.min.x || v.px > mesh.localAabb.max.x ||
            v.py < mesh.localAabb.min.y || v.py > mesh.localAabb.max.y ||
            v.pz < mesh.localAabb.min.z || v.pz > mesh.localAabb.max.z) {
            insideAabb = false;
        }
    }
    CHECK(insideAabb);

    REQUIRE(mesh.materialIndex >= 0);
    REQUIRE(static_cast<std::size_t>(mesh.materialIndex) < s.materials.size());
    const asset::MaterialData& mat = s.materials[static_cast<std::size_t>(mesh.materialIndex)];
    CHECK(mat.baseColorTexture >= 0);
    CHECK(mat.normalTexture >= 0);

    for (const asset::TextureData& tex : s.textures) {
        CHECK(tex.width > 0);
        CHECK(tex.height > 0);
        CHECK(tex.rgba.size() ==
              static_cast<std::size_t>(tex.width) * static_cast<std::size_t>(tex.height) * 4);
    }

    CHECK(s.textures[static_cast<std::size_t>(mat.baseColorTexture)].srgb == true);
    CHECK(s.textures[static_cast<std::size_t>(mat.normalTexture)].srgb == false);

    // Helmet has no dual-use texture, so color-space resolution must not duplicate.
    CHECK(s.textures.size() == 5);
}

TEST_CASE("loadHdr loads the studio environment") {
    std::filesystem::path path =
        std::filesystem::path(KUMO_ASSET_DIR) / "env" / "studio_small_09_2k.hdr";
    auto result = asset::loadHdr(path);
    REQUIRE(result.has_value());
    const asset::HdrImage& img = *result;

    CHECK(img.width == 2048);
    CHECK(img.height == 1024);
    CHECK(img.rgba.size() ==
          static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4);

    bool hasHdr = false;
    for (float v : img.rgba) {
        if (v > 1.0f) {
            hasHdr = true;
            break;
        }
    }
    CHECK(hasHdr);
}

namespace {

std::vector<std::uint8_t> makeGradientRgba(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            rgba[i + 0] =
                static_cast<std::uint8_t>(x * 255 / std::max<std::uint32_t>(1, width - 1));
            rgba[i + 1] =
                static_cast<std::uint8_t>(y * 255 / std::max<std::uint32_t>(1, height - 1));
            rgba[i + 2] = 128;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

} // namespace

TEST_CASE("downscaleRgba scales the long side down while preserving aspect ratio") {
    const std::uint32_t width = 8;
    const std::uint32_t height = 4;
    const std::vector<std::uint8_t> src = makeGradientRgba(width, height);

    const asset::DownscaledImage out = asset::downscaleRgba(src.data(), width, height, 4);
    CHECK(out.width == 4);
    CHECK(out.height == 2);
    // uint8 output is trivially finite; the size check is what actually matters.
    REQUIRE(out.rgba.size() == static_cast<std::size_t>(out.width) * out.height * 4);
}

TEST_CASE("downscaleRgba passes through unchanged when already within the budget") {
    const std::uint32_t width = 4;
    const std::uint32_t height = 3;
    const std::vector<std::uint8_t> src = makeGradientRgba(width, height);

    const asset::DownscaledImage out = asset::downscaleRgba(src.data(), width, height, 8);
    CHECK(out.width == width);
    CHECK(out.height == height);
    REQUIRE(out.rgba.size() == src.size());
    CHECK(out.rgba == src);
}

TEST_CASE("downscaleRgba is deterministic across repeated calls") {
    const std::uint32_t width = 37;
    const std::uint32_t height = 21;
    const std::vector<std::uint8_t> src = makeGradientRgba(width, height);

    const asset::DownscaledImage first = asset::downscaleRgba(src.data(), width, height, 16);
    const asset::DownscaledImage second = asset::downscaleRgba(src.data(), width, height, 16);
    CHECK(first.width == second.width);
    CHECK(first.height == second.height);
    CHECK(first.rgba == second.rgba);
}

TEST_CASE("loaders report missing files with the path") {
    auto gltf = asset::loadGltf("/no/such/model.glb");
    REQUIRE_FALSE(gltf.has_value());
    CHECK(gltf.error().find("/no/such/model.glb") != std::string::npos);

    auto hdr = asset::loadHdr("/no/such/image.hdr");
    REQUIRE_FALSE(hdr.has_value());
    CHECK(hdr.error().find("/no/such/image.hdr") != std::string::npos);
}
