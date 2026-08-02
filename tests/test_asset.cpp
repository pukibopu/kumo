#include <doctest/doctest.h>

#include <kumo/asset/asset.h>
#include <kumo/asset/model_resolver.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace kumo;

namespace {

// Removed on destruction; each test gets a fresh, non-colliding directory
// (mirrors test_agent_scene_tools.cpp's TempDir, duplicated locally since it
// is private to that translation unit).
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).put('x');
}

} // namespace

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

// --- resolveModelPath / listModelIds (MA milestone) -------------------------

TEST_CASE("resolveModelPath prefers <name>.glb over the multi-file layouts") {
    TempDir dir("kumo_model_resolver_glb");
    touch(dir.path / "Avocado.glb");
    touch(dir.path / "Avocado" / "scene.gltf"); // must lose to the .glb hit

    const std::filesystem::path resolved = asset::resolveModelPath(dir.path, "Avocado");
    CHECK(resolved == dir.path / "Avocado.glb");
}

TEST_CASE("resolveModelPath falls back to <name>/<name>.gltf then <name>/scene.gltf") {
    TempDir dir("kumo_model_resolver_selfnamed");
    touch(dir.path / "barrel" / "barrel.gltf");
    CHECK(asset::resolveModelPath(dir.path, "barrel") == dir.path / "barrel" / "barrel.gltf");

    TempDir dir2("kumo_model_resolver_scenegltf");
    touch(dir2.path / "barrel" / "scene.gltf");
    CHECK(asset::resolveModelPath(dir2.path, "barrel") == dir2.path / "barrel" / "scene.gltf");
}

TEST_CASE("resolveModelPath resolves a category/name path through all three category layouts") {
    TempDir glbDir("kumo_model_resolver_cat_glb");
    touch(glbDir.path / "nature" / "bridge_stone.glb");
    CHECK(asset::resolveModelPath(glbDir.path, "nature/bridge_stone") ==
          glbDir.path / "nature" / "bridge_stone.glb");

    TempDir selfDir("kumo_model_resolver_cat_selfnamed");
    touch(selfDir.path / "nature" / "bridge_stone" / "bridge_stone.gltf");
    CHECK(asset::resolveModelPath(selfDir.path, "nature/bridge_stone") ==
          selfDir.path / "nature" / "bridge_stone" / "bridge_stone.gltf");

    TempDir sceneDir("kumo_model_resolver_cat_scenegltf");
    touch(sceneDir.path / "nature" / "bridge_stone" / "scene.gltf");
    CHECK(asset::resolveModelPath(sceneDir.path, "nature/bridge_stone") ==
          sceneDir.path / "nature" / "bridge_stone" / "scene.gltf");
}

TEST_CASE("resolveModelPath returns an empty path when nothing on disk matches") {
    TempDir dir("kumo_model_resolver_miss");
    CHECK(asset::resolveModelPath(dir.path, "nothing_here").empty());
    CHECK(asset::resolveModelPath(dir.path, "props/nothing_here").empty());
    // A non-existent modelsDir is just another kind of miss, not an error.
    CHECK(asset::resolveModelPath(dir.path / "does_not_exist", "Avocado").empty());
}

TEST_CASE("resolveModelPath keeps the documented priority for categorized models") {
    // Both multi-file layouts present at once: the self-named .gltf must win
    // over scene.gltf, exactly as in the uncategorized case.
    TempDir dir("kumo_model_resolver_cat_priority");
    touch(dir.path / "nature" / "bridge_stone" / "bridge_stone.gltf");
    touch(dir.path / "nature" / "bridge_stone" / "scene.gltf");
    CHECK(asset::resolveModelPath(dir.path, "nature/bridge_stone") ==
          dir.path / "nature" / "bridge_stone" / "bridge_stone.gltf");

    // The garbage candidate a naive string append would produce must never
    // resolve, even when a file actually sits at that path.
    TempDir garbage("kumo_model_resolver_cat_garbage");
    touch(garbage.path / "nature" / "bridge_stone" / "nature" / "bridge_stone.gltf");
    CHECK(asset::resolveModelPath(garbage.path, "nature/bridge_stone").empty());
}

TEST_CASE("resolveModelPath rejects names with more than one category level") {
    TempDir dir("kumo_model_resolver_deep");
    touch(dir.path / "a" / "b" / "c.glb");
    CHECK(asset::resolveModelPath(dir.path, "a/b/c").empty());
}

TEST_CASE("listModelIds reports an id existing in several layouts exactly once") {
    TempDir dir("kumo_model_resolver_dedup");
    touch(dir.path / "Avocado.glb");
    touch(dir.path / "Avocado" / "scene.gltf");
    touch(dir.path / "props" / "crate.glb");
    touch(dir.path / "props" / "crate" / "crate.gltf");

    const std::vector<std::string> ids = asset::listModelIds(dir.path);
    const std::vector<std::string> expected{"Avocado", "props/crate"};
    CHECK(ids == expected);
}

TEST_CASE("listModelIds enumerates flat glb, self-named multi-file and category models, sorted") {
    TempDir dir("kumo_model_resolver_list");
    touch(dir.path / "Avocado.glb");
    touch(dir.path / "dikhololo_barrel" /
          "scene.gltf");                             // uncategorized multi-file (fetchModel-style)
    touch(dir.path / "nature" / "bridge_stone.glb"); // category flat glb (fetch_pack-style)
    touch(dir.path / "nature" / "cactus.glb");
    touch(dir.path / "nature" / "pack.json");           // manifest file, not a model
    touch(dir.path / "props" / "crate" / "crate.gltf"); // category self-named multi-file

    const std::vector<std::string> ids = asset::listModelIds(dir.path);
    const std::vector<std::string> expected{"Avocado", "dikhololo_barrel", "nature/bridge_stone",
                                            "nature/cactus", "props/crate"};
    CHECK(ids == expected);
}

TEST_CASE("listModelIds on a missing modelsDir is empty, not an error") {
    CHECK(asset::listModelIds("/no/such/models/dir").empty());
}
