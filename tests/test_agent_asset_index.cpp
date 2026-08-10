#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), not a public interface;
// reached into here the same way test_agent_asset_fetch.cpp reaches into
// asset_fetch.h.
#include "asset_index.h"

#include <kumo/math/math.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

// Removed on destruction; each test gets a fresh, non-colliding directory
// (mirrors test_agent_scene_tools.cpp's TempDir).
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

AssetIndex sampleIndex() {
    AssetIndex index;
    index.version = 1;
    index.generated = "test-run";
    index.embedding =
        AssetIndexEmbedding{.model = "clip-vit", .dim = 512, .file = "embeddings.bin"};

    AssetIndexEntry texture;
    texture.id = "sand";
    texture.kind = AssetIndexKind::Texture;
    texture.name = "Sand";
    texture.style = "realistic";
    texture.license = "CC0";
    texture.source = "ambientCG (Ground037)";
    texture.tags = {"ground", "outdoor"};
    texture.maps = {"albedo", "normal", "roughness"};
    texture.resolution = 1024;
    texture.thumbnail = ".thumbnails/textures/sand.png";
    index.entries.push_back(texture);

    AssetIndexEntry model;
    model.id = "nature/bridge_stone";
    model.kind = AssetIndexKind::Model;
    model.category = "nature";
    model.style = "stylized";
    model.license = "CC0";
    model.source = "Kenney (Nature Kit)";
    model.dimensions = math::float3{1.2f, 0.4f, 2.5f};
    model.triangles = 240;
    model.instancingOk = true;
    model.thumbnail = ".thumbnails/models/nature/bridge_stone.png";
    index.entries.push_back(model);

    AssetIndexEntry env;
    env.id = "day";
    env.kind = AssetIndexKind::Environment;
    env.style = "realistic";
    env.license = "CC0";
    env.source = "Poly Haven";
    env.thumbnail = ".thumbnails/env/day.png";
    index.entries.push_back(env);

    return index;
}

} // namespace

TEST_CASE("AssetIndex round-trips every kind's fields through serialize/parse") {
    const AssetIndex original = sampleIndex();
    const std::string text = serializeAssetIndex(original);
    const std::optional<AssetIndex> parsed = parseAssetIndex(text);
    REQUIRE(parsed.has_value());

    CHECK(parsed->version == original.version);
    CHECK(parsed->generated == original.generated);
    REQUIRE(parsed->embedding.has_value());
    CHECK(parsed->embedding->model == "clip-vit");
    CHECK(parsed->embedding->dim == 512);
    CHECK(parsed->embedding->file == "embeddings.bin");

    REQUIRE(parsed->entries.size() == 3);
    const AssetIndexEntry& texture = parsed->entries[0];
    CHECK(texture.id == "sand");
    CHECK(texture.kind == AssetIndexKind::Texture);
    CHECK(texture.name == "Sand");
    CHECK(texture.tags == std::vector<std::string>{"ground", "outdoor"});
    CHECK(texture.maps == std::vector<std::string>{"albedo", "normal", "roughness"});
    REQUIRE(texture.resolution.has_value());
    CHECK(*texture.resolution == 1024);
    CHECK(!texture.dimensions.has_value());
    CHECK(!texture.triangles.has_value());
    CHECK(texture.embeddingOffset == -1);

    const AssetIndexEntry& model = parsed->entries[1];
    CHECK(model.id == "nature/bridge_stone");
    CHECK(model.kind == AssetIndexKind::Model);
    CHECK(model.category == "nature");
    CHECK(model.style == "stylized");
    REQUIRE(model.dimensions.has_value());
    CHECK(model.dimensions->x == doctest::Approx(1.2f));
    CHECK(model.dimensions->y == doctest::Approx(0.4f));
    CHECK(model.dimensions->z == doctest::Approx(2.5f));
    REQUIRE(model.triangles.has_value());
    CHECK(*model.triangles == 240);
    REQUIRE(model.instancingOk.has_value());
    CHECK(*model.instancingOk);
    CHECK(model.maps.empty());
    CHECK(!model.resolution.has_value());

    const AssetIndexEntry& env = parsed->entries[2];
    CHECK(env.id == "day");
    CHECK(env.kind == AssetIndexKind::Environment);
    CHECK(!env.dimensions.has_value());
    CHECK(!env.resolution.has_value());
    CHECK(env.maps.empty());
}

TEST_CASE("parseAssetIndex defaults every optional field an entry omits") {
    constexpr const char* kMinimal =
        R"({"version":1,"generated":"","entries":[{"id":"bare","kind":"texture"}]})";
    const std::optional<AssetIndex> parsed = parseAssetIndex(kMinimal);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entries.size() == 1);
    const AssetIndexEntry& entry = parsed->entries.front();
    CHECK(entry.id == "bare");
    CHECK(entry.kind == AssetIndexKind::Texture);
    CHECK(entry.name.empty());
    CHECK(entry.category.empty());
    CHECK(entry.style.empty());
    CHECK(entry.license.empty());
    CHECK(entry.source.empty());
    CHECK(entry.tags.empty());
    CHECK(entry.caption.empty());
    CHECK(entry.thumbnail.empty());
    CHECK(entry.maps.empty());
    CHECK(!entry.resolution.has_value());
    CHECK(!entry.dimensions.has_value());
    CHECK(!entry.triangles.has_value());
    CHECK(!entry.instancingOk.has_value());
    CHECK(entry.embeddingOffset == -1);
    CHECK(!parsed->embedding.has_value());
}

TEST_CASE("parseAssetIndex defaults a missing top-level version/generated/entries") {
    const std::optional<AssetIndex> parsed = parseAssetIndex("{}");
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->generated.empty());
    CHECK(parsed->entries.empty());
    CHECK(!parsed->embedding.has_value());
}

TEST_CASE("parseAssetIndex is lenient: unknown top-level and entry fields are ignored") {
    constexpr const char* kExtra =
        R"({"version":2,"generated":"now","unknown_top_level":123,
"entries":[{"id":"x","kind":"model","unknown_entry_field":"whatever","triangles":10}]})";
    const std::optional<AssetIndex> parsed = parseAssetIndex(kExtra);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 2);
    REQUIRE(parsed->entries.size() == 1);
    CHECK(parsed->entries.front().id == "x");
    REQUIRE(parsed->entries.front().triangles.has_value());
    CHECK(*parsed->entries.front().triangles == 10);
}

TEST_CASE(
    "parseAssetIndex skips an entry missing id or with an unrecognized kind, keeps the rest") {
    constexpr const char* kMixed = R"({"version":1,"generated":"","entries":[
{"id":"","kind":"texture"},
{"kind":"texture"},
{"id":"weird","kind":"not_a_real_kind"},
{"id":"good","kind":"env"}
]})";
    const std::optional<AssetIndex> parsed = parseAssetIndex(kMixed);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entries.size() == 1);
    CHECK(parsed->entries.front().id == "good");
    CHECK(parsed->entries.front().kind == AssetIndexKind::Environment);
}

TEST_CASE("parseAssetIndex rejects malformed JSON and a non-object top level") {
    CHECK(!parseAssetIndex("not json").has_value());
    CHECK(!parseAssetIndex("[]").has_value());
    CHECK(!parseAssetIndex("\"just a string\"").has_value());
}

TEST_CASE(
    "loadAssetIndex reads a written index back, and is nullopt on a missing or corrupt file") {
    TempDir dir("kumo_asset_index_load");
    CHECK(!loadAssetIndex(dir.path).has_value()); // no index.json yet

    const AssetIndex original = sampleIndex();
    REQUIRE(saveAssetIndex(dir.path, original));
    const std::optional<AssetIndex> loaded = loadAssetIndex(dir.path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->entries.size() == original.entries.size());
    CHECK(loaded->generated == original.generated);

    writeFile(dir.path / "index.json", "{ not valid json");
    CHECK(!loadAssetIndex(dir.path).has_value());
}

TEST_CASE("saveAssetIndex creates the asset directory when it does not exist yet") {
    TempDir parent("kumo_asset_index_save_missing_dir");
    const std::filesystem::path assetDir = parent.path / "fresh";
    REQUIRE(!std::filesystem::exists(assetDir));
    CHECK(saveAssetIndex(assetDir, sampleIndex()));
    CHECK(std::filesystem::exists(assetDir / "index.json"));
}

TEST_CASE("recipe and spec kinds round-trip through serialize/parse (MR)") {
    AssetIndex index;
    AssetIndexEntry recipe;
    recipe.id = "wood_grain";
    recipe.kind = AssetIndexKind::Recipe;
    recipe.caption = "Procedural wood rings";
    recipe.tags = {"wood", "natural"};
    recipe.thumbnail = ".thumbnails/recipes/wood_grain.png";
    recipe.embeddingOffset = 3;
    index.entries.push_back(recipe);
    AssetIndexEntry spec;
    spec.id = "product_studio";
    spec.kind = AssetIndexKind::Spec;
    spec.caption = "Studio shot";
    spec.style = "realistic";
    index.entries.push_back(spec);

    const std::optional<AssetIndex> loaded = parseAssetIndex(serializeAssetIndex(index));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[0].kind == AssetIndexKind::Recipe);
    CHECK(loaded->entries[0].embeddingOffset == 3);
    CHECK(loaded->entries[1].kind == AssetIndexKind::Spec);
    CHECK(loaded->entries[1].caption == "Studio shot");
}

TEST_CASE("embedding sidecar rows round-trip and reject a torn file (MR)") {
    TempDir dir("kumo_asset_index_sidecar");
    const AssetIndexEmbedding embedding{.model = "test-embed", .dim = 4, .file = "rows.bin"};
    const std::vector<float> rows{1.0f, 2.0f, 3.0f, 4.0f, -1.0f, 0.5f, 0.25f, 8.0f};
    REQUIRE(saveEmbeddingRows(dir.path, embedding, rows));

    const std::optional<std::vector<float>> loaded = loadEmbeddingRows(dir.path, embedding);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == rows);

    // Not a whole number of dim-sized rows: torn write or dim drift.
    writeFile(dir.path / "rows.bin", "123456");
    CHECK(!loadEmbeddingRows(dir.path, embedding).has_value());
    CHECK(!loadEmbeddingRows(dir.path, {.model = "m", .dim = 0, .file = "rows.bin"}).has_value());
    CHECK(!loadEmbeddingRows(dir.path, {.model = "m", .dim = 4, .file = "absent.bin"}).has_value());
}

TEST_CASE("saveEmbeddingRows rejects a row/dim mismatch (MR)") {
    TempDir dir("kumo_asset_index_sidecar_mismatch");
    CHECK(!saveEmbeddingRows(dir.path, {.model = "m", .dim = 4, .file = "rows.bin"},
                             std::vector<float>{1.0f, 2.0f, 3.0f}));
    CHECK(!std::filesystem::exists(dir.path / "rows.bin"));
}
