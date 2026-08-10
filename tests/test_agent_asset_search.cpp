#include <doctest/doctest.h>

// Private kumo_agent headers (engine/agent/src), same pattern as
// test_agent_asset_index.cpp.
#include "asset_index.h"
#include "asset_search.h"

#include <kumo/math/math.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace kumo;
using namespace kumo::agent;
namespace search = kumo::agent::search;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

AssetIndexEntry makeEntry(const char* id, AssetIndexKind kind) {
    AssetIndexEntry entry;
    entry.id = id;
    entry.kind = kind;
    return entry;
}

// Three entries with a 2d embedding sidecar: asphalt points along x, grass
// along y, brick diagonally.
AssetIndex embeddedIndex() {
    AssetIndex index;
    index.embedding = AssetIndexEmbedding{.model = "test", .dim = 2, .file = "rows.bin"};
    AssetIndexEntry asphalt = makeEntry("asphalt", AssetIndexKind::Texture);
    asphalt.tags = {"road", "urban"};
    asphalt.embeddingOffset = 0;
    AssetIndexEntry grass = makeEntry("grass", AssetIndexKind::Texture);
    grass.tags = {"ground", "nature"};
    grass.embeddingOffset = 1;
    AssetIndexEntry brick = makeEntry("brick", AssetIndexKind::Texture);
    brick.tags = {"wall", "urban"};
    brick.embeddingOffset = 2;
    index.entries = {asphalt, grass, brick};
    return index;
}

const std::vector<float> kRows{1.0f, 0.0f, 0.0f, 1.0f, 0.7f, 0.7f};

} // namespace

TEST_CASE("tokenizeQuery lowercases, splits on punctuation and keeps CJK bytes as one token") {
    CHECK(search::tokenizeQuery("Wet Asphalt, night-city!") ==
          std::vector<std::string>{"wet", "asphalt", "night", "city"});
    CHECK(search::tokenizeQuery("  ") == std::vector<std::string>{});
    // UTF-8 CJK bytes are word characters: the phrase survives as one token
    // instead of dissolving into nothing.
    CHECK(search::tokenizeQuery("赛博街道 wet") == std::vector<std::string>{"赛博街道", "wet"});
}

TEST_CASE("passesFilters enforces the kind whitelist, case-insensitive category/style and "
          "max_dimension") {
    AssetIndexEntry entry = makeEntry("crate", AssetIndexKind::Model);
    entry.category = "Props";
    entry.style = "Stylized";
    entry.dimensions = math::float3{0.5f, 2.0f, 0.5f};

    CHECK(search::passesFilters(entry, {}));
    CHECK(search::passesFilters(entry, {.kinds = {AssetIndexKind::Model}}));
    CHECK(!search::passesFilters(entry, {.kinds = {AssetIndexKind::Texture}}));
    CHECK(search::passesFilters(entry, {.category = "props"}));
    CHECK(!search::passesFilters(entry, {.category = "nature"}));
    CHECK(search::passesFilters(entry, {.style = "stylized"}));
    CHECK(!search::passesFilters(entry, {.style = "realistic"}));
    CHECK(search::passesFilters(entry, {.maxDimension = 2.5f}));
    CHECK(!search::passesFilters(entry, {.maxDimension = 1.0f}));
    // No dimensions recorded: the constraint cannot be evaluated, so it passes.
    entry.dimensions.reset();
    CHECK(search::passesFilters(entry, {.maxDimension = 0.1f}));
}

TEST_CASE("ftsScore prefers id/name over tags over caption over category and normalizes to "
          "[0,1]") {
    AssetIndexEntry entry = makeEntry("asphalt_wet", AssetIndexKind::Texture);
    entry.tags = {"road"};
    entry.caption = "a rainy street surface";
    entry.category = "urban";

    const std::vector<std::string> idHit{"asphalt"};
    const std::vector<std::string> tagHit{"road"};
    const std::vector<std::string> captionHit{"rainy"};
    const std::vector<std::string> categoryHit{"urban"};
    const std::vector<std::string> miss{"forest"};
    const float id = search::ftsScore(entry, idHit);
    const float tag = search::ftsScore(entry, tagHit);
    const float caption = search::ftsScore(entry, captionHit);
    const float category = search::ftsScore(entry, categoryHit);
    CHECK(id == 1.0f);
    CHECK(id > tag);
    CHECK(tag > caption);
    CHECK(caption > category);
    CHECK(category > 0.0f);
    CHECK(search::ftsScore(entry, miss) == 0.0f);
    CHECK(search::ftsScore(entry, {}) == 0.0f);
    // Two tokens, one hitting the id, one missing: half the id tier.
    const std::vector<std::string> half{"asphalt", "forest"};
    CHECK(search::ftsScore(entry, half) == 0.5f);
}

TEST_CASE("cosine handles identity, orthogonality, mismatch and zero norms") {
    const std::vector<float> x{1.0f, 0.0f};
    const std::vector<float> y{0.0f, 1.0f};
    const std::vector<float> zero{0.0f, 0.0f};
    const std::vector<float> three{1.0f, 0.0f, 0.0f};
    CHECK(search::cosine(x, x) == doctest::Approx(1.0f));
    CHECK(search::cosine(x, y) == doctest::Approx(0.0f));
    CHECK(search::cosine(x, zero) == 0.0f);
    CHECK(search::cosine(x, three) == 0.0f);
    CHECK(search::cosine({}, {}) == 0.0f);
}

TEST_CASE("searchIndex with FTS only ranks hits and drops non-matches") {
    AssetIndex index;
    index.entries = {makeEntry("asphalt", AssetIndexKind::Texture),
                     makeEntry("grass", AssetIndexKind::Texture),
                     makeEntry("asphalt_snow", AssetIndexKind::Texture)};
    const std::vector<std::string> tokens{"asphalt"};
    const std::vector<search::Match> matches = search::searchIndex(index, {}, {}, tokens, {}, 5);
    REQUIRE(matches.size() == 2);
    // Equal tier hits tie; ids break the tie deterministically.
    CHECK(index.entries[matches[0].entryIndex].id == "asphalt");
    CHECK(index.entries[matches[1].entryIndex].id == "asphalt_snow");
}

TEST_CASE("searchIndex with a query vector alone ranks by cosine") {
    const AssetIndex index = embeddedIndex();
    const std::vector<float> queryVec{0.0f, 1.0f}; // toward grass
    const std::vector<search::Match> matches =
        search::searchIndex(index, kRows, {}, {}, queryVec, 5);
    REQUIRE(matches.size() == 3);
    CHECK(index.entries[matches[0].entryIndex].id == "grass");
    CHECK(index.entries[matches[1].entryIndex].id == "brick");
}

TEST_CASE("searchIndex fuses FTS and cosine by reciprocal rank") {
    AssetIndex index = embeddedIndex();
    // asphalt leads the FTS ranking only (its embedding is gone), grass leads
    // the cosine ranking only; brick places second in BOTH rankings and must
    // win the fusion over each single-list leader.
    index.entries[0].embeddingOffset = -1;
    const std::vector<std::string> tokens{"urban"};
    const std::vector<float> queryVec{0.1f, 1.0f};
    const std::vector<search::Match> matches =
        search::searchIndex(index, kRows, {}, tokens, queryVec, 5);
    REQUIRE(matches.size() == 3);
    CHECK(index.entries[matches[0].entryIndex].id == "brick");
    // The two one-list leaders tie at 1/(k+1); ids break the tie.
    CHECK(index.entries[matches[1].entryIndex].id == "asphalt");
    CHECK(index.entries[matches[2].entryIndex].id == "grass");
}

TEST_CASE("searchIndex skips entries whose embedding offset overruns the sidecar") {
    AssetIndex index = embeddedIndex();
    index.entries[2].embeddingOffset = 9; // row 9 of a 3-row sidecar
    const std::vector<float> queryVec{0.7f, 0.7f};
    const std::vector<search::Match> matches =
        search::searchIndex(index, kRows, {}, {}, queryVec, 5);
    // brick's vector is unreadable and it has no FTS signal, so it drops.
    REQUIRE(matches.size() == 2);
    CHECK(index.entries[matches[0].entryIndex].id != "brick");
    CHECK(index.entries[matches[1].entryIndex].id != "brick");
}

TEST_CASE("searchIndex browse mode (no query signals) returns filtered entries in id order and "
          "clamps limit") {
    AssetIndex index = embeddedIndex();
    index.entries.push_back(makeEntry("wood_grain", AssetIndexKind::Recipe));
    const search::Filters textures{.kinds = {AssetIndexKind::Texture}};
    const std::vector<search::Match> all = search::searchIndex(index, {}, textures, {}, {}, 99);
    REQUIRE(all.size() == 3);
    CHECK(index.entries[all[0].entryIndex].id == "asphalt");
    CHECK(index.entries[all[2].entryIndex].id == "grass");
    CHECK(search::searchIndex(index, {}, textures, {}, {}, -5).size() == 1);
    const std::vector<std::string> tokens{"wood"};
    const std::vector<search::Match> recipeHit = search::searchIndex(index, {}, {}, tokens, {}, 5);
    REQUIRE(recipeHit.size() == 1);
    CHECK(index.entries[recipeHit[0].entryIndex].kind == AssetIndexKind::Recipe);
}

TEST_CASE("refreshSearchCacheLocked loads index and sidecar, and reloads on an mtime change") {
    TempDir dir("kumo_asset_search_cache");
    search::SearchCache cache;
    std::lock_guard<std::mutex> lock(cache.mutex);

    search::refreshSearchCacheLocked(cache, dir.path);
    CHECK(cache.loaded);
    CHECK(!cache.index.has_value());

    AssetIndex index = embeddedIndex();
    REQUIRE(saveAssetIndex(dir.path, index));
    REQUIRE(saveEmbeddingRows(dir.path, *index.embedding, kRows));
    search::refreshSearchCacheLocked(cache, dir.path);
    REQUIRE(cache.index.has_value());
    CHECK(cache.index->entries.size() == 3);
    CHECK(cache.rows == kRows);

    // A rebuilt index (newer mtime) is picked up without a restart.
    index.entries.push_back(makeEntry("new_asset", AssetIndexKind::Model));
    REQUIRE(saveAssetIndex(dir.path, index));
    std::filesystem::last_write_time(dir.path / "index.json",
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::seconds(2));
    search::refreshSearchCacheLocked(cache, dir.path);
    REQUIRE(cache.index.has_value());
    CHECK(cache.index->entries.size() == 4);
}
