#include <doctest/doctest.h>

#include <kumo/asset/texture_set.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace kumo;

namespace {

// One flat-colored RGBA8 image; component values are replicated across all
// four channels so reading any single channel back is unambiguous.
std::vector<std::uint8_t> flatPixels(std::uint32_t width, std::uint32_t height,
                                     std::uint8_t value) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = value;
        pixels[i + 1] = value;
        pixels[i + 2] = value;
        pixels[i + 3] = 255;
    }
    return pixels;
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("loadTextureSet requires albedo") {
    TempDir dir("kumo_texture_set_no_albedo");
    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("albedo") != std::string::npos);
}

TEST_CASE("loadTextureSet loads albedo, normal, ao and packs roughness+metalness") {
    TempDir dir("kumo_texture_set_full");
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 200);
    REQUIRE(asset::writePng(dir.path / "albedo.png", 2, 2, albedo.data()));
    const std::vector<std::uint8_t> normal = flatPixels(2, 2, 128);
    REQUIRE(asset::writePng(dir.path / "normal.png", 2, 2, normal.data()));
    const std::vector<std::uint8_t> ao = flatPixels(2, 2, 220);
    REQUIRE(asset::writePng(dir.path / "ao.png", 2, 2, ao.data()));
    const std::vector<std::uint8_t> roughness = flatPixels(2, 2, 90);
    REQUIRE(asset::writePng(dir.path / "roughness.png", 2, 2, roughness.data()));
    const std::vector<std::uint8_t> metalness = flatPixels(2, 2, 10);
    REQUIRE(asset::writePng(dir.path / "metalness.png", 2, 2, metalness.data()));

    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(result.has_value());
    const asset::TextureSetData& set = *result;

    REQUIRE(set.baseColor.has_value());
    CHECK(set.baseColor->srgb == true);
    CHECK(set.baseColor->width == 2);
    CHECK(set.baseColor->rgba[0] == 200);

    REQUIRE(set.normal.has_value());
    CHECK(set.normal->srgb == false);
    CHECK(set.normal->rgba[0] == 128);

    REQUIRE(set.occlusion.has_value());
    CHECK(set.occlusion->srgb == false);
    CHECK(set.occlusion->rgba[0] == 220);

    REQUIRE(set.metallicRoughness.has_value());
    CHECK(set.metallicRoughness->srgb == false);
    CHECK(set.metallicRoughness->width == 2);
    CHECK(set.metallicRoughness->height == 2);
    // G = roughness, B = metallic.
    CHECK(set.metallicRoughness->rgba[1] == 90);
    CHECK(set.metallicRoughness->rgba[2] == 10);
}

TEST_CASE("loadTextureSet: missing metalness packs as 0") {
    TempDir dir("kumo_texture_set_no_metalness");
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 200);
    REQUIRE(asset::writePng(dir.path / "albedo.png", 2, 2, albedo.data()));
    const std::vector<std::uint8_t> roughness = flatPixels(2, 2, 77);
    REQUIRE(asset::writePng(dir.path / "roughness.png", 2, 2, roughness.data()));

    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(result.has_value());
    REQUIRE(result->metallicRoughness.has_value());
    CHECK(result->metallicRoughness->rgba[1] == 77);
    CHECK(result->metallicRoughness->rgba[2] == 0);
}

TEST_CASE("loadTextureSet: no roughness or metalness leaves metallicRoughness unset") {
    TempDir dir("kumo_texture_set_bare");
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 200);
    REQUIRE(asset::writePng(dir.path / "albedo.png", 2, 2, albedo.data()));

    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(result.has_value());
    CHECK(!result->metallicRoughness.has_value());
    CHECK(!result->normal.has_value());
    CHECK(!result->occlusion.has_value());
}

TEST_CASE("loadTextureSet: mismatched roughness/metalness sizes fail") {
    TempDir dir("kumo_texture_set_mismatch");
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 200);
    REQUIRE(asset::writePng(dir.path / "albedo.png", 2, 2, albedo.data()));
    const std::vector<std::uint8_t> roughness = flatPixels(2, 2, 77);
    REQUIRE(asset::writePng(dir.path / "roughness.png", 2, 2, roughness.data()));
    const std::vector<std::uint8_t> metalness = flatPixels(4, 4, 10);
    REQUIRE(asset::writePng(dir.path / "metalness.png", 4, 4, metalness.data()));

    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("mismatch") != std::string::npos);
}

TEST_CASE("loadTextureSet loads a fetched ambientCG texture set when present") {
    // KUMO_ASSET_DIR/textures/{rock,planks} only exist after
    // tools/fetch_assets.sh has been run locally; CI and a fresh checkout
    // never have them, so this is a bonus check, not a required one.
    const std::filesystem::path rockDir =
        std::filesystem::path(KUMO_ASSET_DIR) / "textures" / "rock";
    if (!std::filesystem::exists(rockDir / "albedo.jpg")) {
        return;
    }
    const auto rock = asset::loadTextureSet(rockDir);
    REQUIRE(rock.has_value());
    CHECK(rock->baseColor.has_value());
    CHECK(rock->baseColor->srgb == true);
    CHECK(rock->normal.has_value());
    CHECK(rock->occlusion.has_value());
    CHECK(rock->metallicRoughness.has_value()); // rock ships a roughness map, no metalness

    const std::filesystem::path planksDir =
        std::filesystem::path(KUMO_ASSET_DIR) / "textures" / "planks";
    if (!std::filesystem::exists(planksDir / "albedo.jpg")) {
        return;
    }
    const auto planks = asset::loadTextureSet(planksDir);
    REQUIRE(planks.has_value());
    REQUIRE(planks->metallicRoughness.has_value()); // planks ships both roughness and metalness
}

TEST_CASE("loadTextureSet probes the .jpg extension when .png is absent") {
    TempDir dir("kumo_texture_set_jpg_probe");
    // stb sniffs the format from the file's own bytes, not its extension, so
    // a PNG-encoded file named albedo.jpg still round-trips through the
    // decoder; this exercises the probeFile fallback to ".jpg" specifically.
    const std::vector<std::uint8_t> albedo = flatPixels(2, 2, 150);
    REQUIRE(asset::writePng(dir.path / "albedo.jpg", 2, 2, albedo.data()));

    const auto result = asset::loadTextureSet(dir.path);
    REQUIRE(result.has_value());
    REQUIRE(result->baseColor.has_value());
    CHECK(result->baseColor->rgba[0] == 150);
}
