#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), not a public interface;
// reached into here the same way test_agent_http_provider.cpp reaches into
// retry_after.h and test_renderer_compat.cpp reaches into kumo_renderer's
// shader_load.h.
#include "asset_fetch.h"

#include <kumo/agent/http_provider.h>
#include <kumo/core/file.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

std::string fixtureText(const char* name) {
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / "polyhaven" / name;
    const auto text = readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

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

// Routes GET requests to canned bodies by exact URL; every visited URL is
// recorded in order so tests can assert exactly what did (and did not)
// happen over the wire. A URL with no registered body fails the request,
// standing in for "the real Poly Haven CDN was never asked" in tests that
// must prove a code path makes no such request.
struct RoutedTransport {
    std::unordered_map<std::string, std::string> bodies;
    std::unordered_map<std::string, int> statusOverride;
    std::vector<std::string> requested;

    void set(const std::string& url, std::string body) { bodies[url] = std::move(body); }

    HttpTransport fn() {
        return [this](const HttpRequest& request, std::chrono::seconds,
                      const std::atomic<bool>&) -> std::expected<HttpResponse, TransportError> {
            requested.push_back(request.url);
            CHECK(request.method == "GET");
            const auto bodyIt = bodies.find(request.url);
            if (bodyIt == bodies.end()) {
                return std::unexpected(
                    TransportError{.kind = TransportError::Kind::ConnectionFailed,
                                   .message = "no fixture for " + request.url});
            }
            HttpResponse response;
            const auto statusIt = statusOverride.find(request.url);
            response.status = statusIt != statusOverride.end() ? statusIt->second : 200;
            response.body = bodyIt->second;
            return response;
        };
    }
};

// Extracts one map's url out of a /files/<id>-shaped fixture by slot name
// (e.g. "albedo"), via mapUrls itself; used to register the exact URLs a
// RoutedTransport must answer without hand-copying them from the fixture text.
std::string mapUrlFromFixture(const std::string& filesJson, const char* slot, const char* res,
                              const char* fmt) {
    const std::unordered_map<std::string, std::string> urls =
        *mapUrls(filesJson, AssetKind::Texture, res, fmt);
    const auto it = urls.find(slot);
    REQUIRE(it != urls.end());
    return it->second;
}

} // namespace

// --- pickAsset -----------------------------------------------------------

TEST_CASE("pickAsset ranks an exact substring id hit, tie-breaking on id ascending") {
    const std::string assets = fixtureText("assets_textures.json");
    // asphalt_01, asphalt_02 and aerial_asphalt_01 all contain "asphalt" in
    // their id (same tier); "aerial_asphalt_01" sorts first ('e' < 's').
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "asphalt");
    REQUIRE(result.has_value());
    CHECK(result->id == "aerial_asphalt_01");
    REQUIRE(result->alternatives.size() == 2);
    CHECK(result->alternatives[0] == "asphalt_01");
    CHECK(result->alternatives[1] == "asphalt_02");
}

TEST_CASE("pickAsset picks the alphabetically-first id among equally-ranked snow textures") {
    const std::string assets = fixtureText("assets_textures.json");
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "snow");
    REQUIRE(result.has_value());
    CHECK(result->id == "snow_01");
    REQUIRE(result->alternatives.size() == 2);
    CHECK(result->alternatives[0] == "snow_02");
    CHECK(result->alternatives[1] == "snow_field_aerial");
}

TEST_CASE("pickAsset is an unambiguous single match when only one asset qualifies") {
    const std::string assets = fixtureText("assets_textures.json");
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "metal");
    REQUIRE(result.has_value());
    CHECK(result->id == "metal_plate");
    CHECK(result->alternatives.empty());
}

TEST_CASE("pickAsset over hdris: a multi-word query only the tag+id combination satisfies") {
    const std::string assets = fixtureText("assets_hdris.json");
    // "night" hits many ids; "city" only appears in blue_lagoon_night's tags,
    // so it is the only asset where EVERY query word hits somewhere.
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "night city");
    REQUIRE(result.has_value());
    CHECK(result->id == "blue_lagoon_night");
    CHECK(result->alternatives.empty());
}

TEST_CASE("pickAsset over hdris: single-word query picks the alphabetically-first night id") {
    const std::string assets = fixtureText("assets_hdris.json");
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "night");
    REQUIRE(result.has_value());
    CHECK(result->id == "blue_lagoon_night");
}

TEST_CASE("pickAsset reports near-miss suggestions when no asset matches every word") {
    const std::string assets = fixtureText("assets_textures.json");
    // No single texture in the fixture mentions both "snow" and "asphalt".
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "snow asphalt");
    REQUIRE(!result.has_value());
    CHECK(result.error().find("closest names") != std::string::npos);
    CHECK(result.error().find("aerial_asphalt_01") != std::string::npos);
}

TEST_CASE("pickAsset reports a plain no-match with no suggestions when nothing matches at all") {
    const std::string assets = fixtureText("assets_textures.json");
    const std::expected<AssetMatch, std::string> result = pickAsset(assets, "quixotic");
    REQUIRE(!result.has_value());
    CHECK(result.error().find("quixotic") != std::string::npos);
    CHECK(result.error().find("closest names") == std::string::npos);
}

TEST_CASE("pickAsset rejects malformed JSON and an empty query") {
    CHECK(!pickAsset("not json", "sand").has_value());
    CHECK(!pickAsset("[]", "sand").has_value()); // valid JSON, not an object
    CHECK(!pickAsset(fixtureText("assets_textures.json"), "   ").has_value());
}

TEST_CASE("pickAsset: an exact id match outranks a higher per-word tier sum elsewhere") {
    // Synthetic, hand-built (not a Poly Haven capture): "b_wood" would win a
    // naive per-word-tier sum via its two tag hits, but "a_wood" equals the
    // query exactly, which must dominate regardless of tier sums elsewhere.
    constexpr const char* kAssets =
        R"({"a_wood":{"name":"A Wood","tags":["plank"],"categories":["floor"]},
"b_wood":{"name":"Something Else","tags":["a","wood"],"categories":["a","wood"]}})";
    const std::expected<AssetMatch, std::string> result = pickAsset(kAssets, "a wood");
    REQUIRE(result.has_value());
    CHECK(result->id == "a_wood");
}

// --- mapUrls ---------------------------------------------------------------

TEST_CASE("mapUrls (texture): returns every present map at 1k jpg, correctly renamed to slots") {
    const std::string files = fixtureText("files_metal_plate.json");
    const std::expected<std::unordered_map<std::string, std::string>, std::string> result =
        mapUrls(files, AssetKind::Texture, "1k", "jpg");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 5);
    CHECK(result->at("albedo").find("metal_plate_diff_1k.jpg") != std::string::npos);
    CHECK(result->at("normal").find("metal_plate_nor_gl_1k.jpg") != std::string::npos);
    CHECK(result->at("roughness").find("metal_plate_rough_1k.jpg") != std::string::npos);
    CHECK(result->at("metalness").find("metal_plate_metal_1k.jpg") != std::string::npos);
    CHECK(result->at("ao").find("metal_plate_ao_1k.jpg") != std::string::npos);
    for (const auto& [slot, url] : *result) {
        (void)slot;
        CHECK(isWhitelistedHost(url));
    }
}

TEST_CASE("mapUrls (texture): asset with no Metal map omits metalness, keeps the rest") {
    const std::string files = fixtureText("files_asphalt_02.json");
    const std::expected<std::unordered_map<std::string, std::string>, std::string> result =
        mapUrls(files, AssetKind::Texture, "1k", "jpg");
    REQUIRE(result.has_value());
    CHECK(!result->contains("metalness"));
    CHECK(result->contains("albedo"));
    CHECK(result->contains("normal"));
    CHECK(result->contains("roughness"));
    CHECK(result->contains("ao"));
}

TEST_CASE("mapUrls (texture): missing albedo at a resolution the fixture was trimmed to omit is "
          "an error") {
    const std::string files = fixtureText("files_asphalt_02.json"); // trimmed to 1k/2k only
    const std::expected<std::unordered_map<std::string, std::string>, std::string> result =
        mapUrls(files, AssetKind::Texture, "8k", "jpg");
    REQUIRE(!result.has_value());
    CHECK(result.error().find("albedo") != std::string::npos);
}

TEST_CASE("mapUrls (environment): returns the hdri slot at 2k hdr") {
    const std::string files = fixtureText("files_dikhololo_night.json");
    const std::expected<std::unordered_map<std::string, std::string>, std::string> result =
        mapUrls(files, AssetKind::Environment, "2k", "hdr");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK(result->at("hdri").find("dikhololo_night_2k.hdr") != std::string::npos);
    CHECK(isWhitelistedHost(result->at("hdri")));
}

TEST_CASE("mapUrls (environment): a resolution the fixture was trimmed to omit is an error") {
    const std::string files = fixtureText("files_dikhololo_night.json"); // trimmed to 1k/2k/4k
    const std::expected<std::unordered_map<std::string, std::string>, std::string> result =
        mapUrls(files, AssetKind::Environment, "16k", "hdr");
    REQUIRE(!result.has_value());
    CHECK(result.error().find("16k") != std::string::npos);
}

TEST_CASE("mapUrls rejects malformed JSON") {
    CHECK(!mapUrls("not json", AssetKind::Texture, "1k", "jpg").has_value());
    CHECK(!mapUrls("not json", AssetKind::Environment, "2k", "hdr").has_value());
}

// --- isWhitelistedHost -------------------------------------------------------

TEST_CASE("isWhitelistedHost accepts only the two Poly Haven hosts, https, with a path") {
    CHECK(isWhitelistedHost("https://api.polyhaven.com/assets?t=textures"));
    CHECK(isWhitelistedHost(
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/sand/sand_diff_1k.jpg"));
    CHECK(!isWhitelistedHost("https://evil.example.com/asphalt_02_diff_1k.jpg"));
    // Lookalike host must not pass by prefix alone.
    CHECK(!isWhitelistedHost("https://api.polyhaven.com.evil.tld/assets"));
    CHECK(!isWhitelistedHost("http://api.polyhaven.com/assets")); // not https
    CHECK(!isWhitelistedHost("https://api.polyhaven.com"));       // no trailing '/'
}

// --- PolyHavenClient ---------------------------------------------------------

TEST_CASE("PolyHavenClient::fetchTexture downloads every present map and writes them to disk") {
    TempDir dir("kumo_asset_fetch_texture_new");
    RoutedTransport transport;
    transport.set("https://api.polyhaven.com/assets?t=textures",
                  fixtureText("assets_textures.json"));
    const std::string files = fixtureText("files_metal_plate.json");
    transport.set("https://api.polyhaven.com/files/metal_plate", files);
    transport.set(mapUrlFromFixture(files, "albedo", "1k", "jpg"), "fake-albedo-bytes");
    transport.set(mapUrlFromFixture(files, "normal", "1k", "jpg"), "fake-normal-bytes");
    transport.set(mapUrlFromFixture(files, "roughness", "1k", "jpg"), "fake-roughness-bytes");
    transport.set(mapUrlFromFixture(files, "metalness", "1k", "jpg"), "fake-metalness-bytes");
    transport.set(mapUrlFromFixture(files, "ao", "1k", "jpg"), "fake-ao-bytes");

    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result = client.fetchTexture("metal", dir.path);
    REQUIRE(result.has_value());
    CHECK(result->name == "metal_plate");
    CHECK(!result->alreadyPresent);
    REQUIRE(result->maps.size() == 5);
    CHECK(result->maps[0] == "albedo");
    CHECK(result->maps[4] == "ao");

    const std::filesystem::path setDir = dir.path / "metal_plate";
    for (const char* stem : {"albedo", "normal", "roughness", "metalness", "ao"}) {
        CHECK(std::filesystem::exists(setDir / (std::string(stem) + ".jpg")));
    }
    std::ifstream albedo(setDir / "albedo.jpg", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(albedo)), std::istreambuf_iterator<char>());
    CHECK(content == "fake-albedo-bytes");
}

TEST_CASE("PolyHavenClient::fetchTexture: an existing set short-circuits with zero requests when "
          "the query is already the resolved name") {
    TempDir dir("kumo_asset_fetch_texture_direct_hit");
    std::filesystem::create_directories(dir.path / "sand");
    std::ofstream(dir.path / "sand" / "albedo.jpg").put('x');

    RoutedTransport transport; // deliberately empty: no request must succeed
    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result = client.fetchTexture("sand", dir.path);
    REQUIRE(result.has_value());
    CHECK(result->name == "sand");
    CHECK(result->alreadyPresent);
    CHECK(result->maps == std::vector<std::string>{"albedo"});
    CHECK(transport.requested.empty());
}

TEST_CASE("PolyHavenClient::fetchTexture: a fuzzy query resolving to an already-fetched id skips "
          "the files lookup and downloads") {
    TempDir dir("kumo_asset_fetch_texture_resolved_hit");
    std::filesystem::create_directories(dir.path / "metal_plate");
    std::ofstream(dir.path / "metal_plate" / "albedo.jpg").put('x');

    RoutedTransport transport;
    transport.set("https://api.polyhaven.com/assets?t=textures",
                  fixtureText("assets_textures.json"));
    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result = client.fetchTexture("metal", dir.path);
    REQUIRE(result.has_value());
    CHECK(result->name == "metal_plate");
    CHECK(result->alreadyPresent);
    // Only the assets listing was requested; no /files/metal_plate, no download.
    REQUIRE(transport.requested.size() == 1);
    CHECK(transport.requested[0] == "https://api.polyhaven.com/assets?t=textures");
}

TEST_CASE("PolyHavenClient::fetchTexture rejects a doctored files response pointing off Poly "
          "Haven, before touching the offending host, and writes nothing to disk") {
    TempDir dir("kumo_asset_fetch_texture_doctored");
    RoutedTransport transport;
    transport.set("https://api.polyhaven.com/assets?t=textures",
                  fixtureText("assets_textures.json"));
    transport.set("https://api.polyhaven.com/files/asphalt_02",
                  fixtureText("files_asphalt_02_doctored_host.json"));
    // Deliberately no route for the doctored (non-Poly-Haven) url: if the
    // client ever requested it, the transport would fail the call itself.

    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result =
        client.fetchTexture("asphalt_02", dir.path);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("evil.example.com") != std::string::npos);
    for (const std::string& url : transport.requested) {
        CHECK(url.find("evil.example.com") == std::string::npos);
    }
    CHECK(!std::filesystem::exists(dir.path / "asphalt_02"));
}

TEST_CASE("PolyHavenClient::fetchTexture surfaces a network failure fetching the assets listing") {
    TempDir dir("kumo_asset_fetch_texture_network_error");
    RoutedTransport transport; // no route for the assets listing -> transport fails it
    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result = client.fetchTexture("sand", dir.path);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("network error") != std::string::npos);
}

TEST_CASE("PolyHavenClient::fetchTexture surfaces a no-match error without requesting /files") {
    TempDir dir("kumo_asset_fetch_texture_no_match");
    RoutedTransport transport;
    transport.set("https://api.polyhaven.com/assets?t=textures",
                  fixtureText("assets_textures.json"));
    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result =
        client.fetchTexture("snow asphalt", dir.path);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("closest names") != std::string::npos);
    REQUIRE(transport.requested.size() == 1); // only the assets listing
}

TEST_CASE("PolyHavenClient::fetchEnvironment downloads the 2k hdr and writes <id>.hdr") {
    TempDir dir("kumo_asset_fetch_env_new");
    RoutedTransport transport;
    transport.set("https://api.polyhaven.com/assets?t=hdris", fixtureText("assets_hdris.json"));
    const std::string files = fixtureText("files_dikhololo_night.json");
    transport.set("https://api.polyhaven.com/files/dikhololo_night", files);
    const std::unordered_map<std::string, std::string> hdriUrls =
        *mapUrls(files, AssetKind::Environment, "2k", "hdr");
    transport.set(hdriUrls.at("hdri"), "fake-hdr-bytes");

    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result =
        client.fetchEnvironment("dikhololo night", dir.path);
    REQUIRE(result.has_value());
    CHECK(result->name == "dikhololo_night");
    CHECK(!result->alreadyPresent);
    CHECK(result->maps.empty());
    REQUIRE(std::filesystem::exists(dir.path / "dikhololo_night.hdr"));
    std::ifstream hdr(dir.path / "dikhololo_night.hdr", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(hdr)), std::istreambuf_iterator<char>());
    CHECK(content == "fake-hdr-bytes");
}

TEST_CASE("PolyHavenClient::fetchEnvironment: an existing file short-circuits with zero requests") {
    TempDir dir("kumo_asset_fetch_env_direct_hit");
    std::ofstream(dir.path / "dikhololo_night.hdr").put('x');

    RoutedTransport transport; // deliberately empty
    PolyHavenClient client(transport.fn());
    const std::expected<FetchedAsset, std::string> result =
        client.fetchEnvironment("dikhololo_night", dir.path);
    REQUIRE(result.has_value());
    CHECK(result->alreadyPresent);
    CHECK(transport.requested.empty());
}
