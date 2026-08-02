#pragma once

#include <kumo/agent/http_provider.h>
#include <kumo/agent/scene_tools.h> // FetchedAsset

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kumo::agent {

// True when `url` starts with an allowed Poly Haven host (api.polyhaven.com
// for JSON, dl.polyhaven.org for the actual files); anchored on the trailing
// '/' so a lookalike host ("api.polyhaven.com.evil.tld/...") cannot pass by
// prefix alone. Pure so the whitelist itself is fixture-testable without a
// network stack.
bool isWhitelistedHost(std::string_view url);

// One ranked pick over a Poly Haven /assets?t=... listing.
struct AssetMatch {
    std::string id;
    // Up to 3 runner-up ids, best first; empty when nothing else qualified.
    std::vector<std::string> alternatives;
};

// `assetsJson` is the raw response body of GET /assets?t=textures|hdris. Pure,
// no I/O. `query` is split into lowercase words; a candidate asset qualifies
// when EVERY word appears as a case-insensitive substring somewhere across its
// id, name, tags or categories (any one field per word); qualifying
// candidates are ranked by summing, per word, the best field tier it hit (id >
// name > tag > category), plus a large bonus when the id equals the whole
// query exactly. Ties break on id, so results are deterministic regardless of
// the JSON object's key order.
// On no qualifying asset, the error message lists up to 3 near-miss ids
// (assets that matched at least one word) when any exist, distinguishing a
// genuine no-match from "nothing shares even one word with the query".
std::expected<AssetMatch, std::string> pickAsset(std::string_view assetsJson,
                                                 std::string_view query);

enum class AssetKind { Texture, Environment };

// `filesJson` is the raw response body of GET /files/<id>. Pure, no I/O.
// For AssetKind::Texture: up to five slots (albedo/normal/roughness/
// metalness/ao -> url), read from the Diffuse/nor_gl/Rough/Metal/AO keys at
// `resolution`+`format`; only maps actually present there are included.
// Missing albedo (Diffuse, the only map Poly Haven guarantees) is an error.
// For AssetKind::Environment: one slot ("hdri" -> url), read from the
// hdri/<resolution>/<format> key (format is normally "hdr", Poly Haven's only
// HDRI download format).
std::expected<std::unordered_map<std::string, std::string>, std::string>
mapUrls(std::string_view filesJson, AssetKind kind, std::string_view resolution,
        std::string_view format);

// One multi-file glTF download: the .gltf file itself plus every file its
// "include" map lists (relative path, as referenced by the glTF's own
// buffer/image uris, -> download url). Downloading exactly this set into a
// directory that preserves the relative paths reproduces a working local
// glTF (verified against Poly Haven's real /files/<model id> response: the
// gltf's own buffer/image uris match the include map's keys byte for byte).
struct ModelBundle {
    std::string gltfUrl;
    std::unordered_map<std::string, std::string> include;
};

// `filesJson` is the raw response body of GET /files/<id> for a t=models
// asset. Pure, no I/O. Poly Haven nests a model's glTF bundle one level
// deeper than mapUrls' shapes: files["gltf"][resolution]["gltf"] = {url,
// include}. Tries `resolution` first, then falls back through 1k/2k/4k/8k
// (smallest first, since a thumbnail-grade fetch never needs more) to the
// first one present. Error when the asset has no "gltf" key at all (some
// Poly Haven models only ship blend/fbx/usd) or no resolution has one.
std::expected<ModelBundle, std::string> modelBundle(std::string_view filesJson,
                                                    std::string_view resolution = "1k");

// Poly Haven's public, unauthenticated, CC0-only asset API (M6.99): no auth,
// GET only. `transport` is injected the same way HttpLLMProvider takes one
// (see http_provider.h), so tests replay fixtures instead of touching the
// network; the real caller passes makeUrlSessionTransport().
class PolyHavenClient {
public:
    explicit PolyHavenClient(HttpTransport transport,
                             std::chrono::seconds timeout = std::chrono::seconds{60});

    // Resolves `query` against Poly Haven's texture listing and downloads the
    // 1k jpg maps into `<texturesDir>/<id>/{albedo,normal,roughness,
    // metalness,ao}.jpg` (only the maps Poly Haven has for that asset).
    // Idempotent two ways, both checked before the corresponding request:
    // `query` itself already naming an existing `<texturesDir>/<query>`
    // short-circuits with NO network I/O at all (the common case of an agent
    // re-passing a name a previous asset_fetch call returned); otherwise
    // `query` is resolved against the assets listing (one request) and THEN
    // `<texturesDir>/<id>` existing short-circuits before the files listing
    // and any file download. Either way the result reports
    // alreadyPresent=true. Error on no match (with near-miss suggestions), a
    // disallowed host, an oversized file (64MB cap) or any network/write
    // failure.
    std::expected<FetchedAsset, std::string> fetchTexture(std::string_view query,
                                                          const std::filesystem::path& texturesDir);
    // Same contract, downloading the 2k hdr into `<envDir>/<id>.hdr`.
    std::expected<FetchedAsset, std::string> fetchEnvironment(std::string_view query,
                                                              const std::filesystem::path& envDir);
    // Same idempotency/error contract as fetchTexture/fetchEnvironment, but the
    // download itself is a multi-file glTF bundle (modelBundle) written first to
    // a temp sibling directory (`<modelsDir>/.partial_<id>`) and only renamed
    // onto `<modelsDir>/<id>/` once every file is down: the gltf renamed to
    // "scene.gltf" (so it matches the resolver's <name>/scene.gltf rule
    // regardless of Poly Haven's own resolution-suffixed filename) plus every
    // include file at its relative path. A failure at any point (network or
    // disk) removes the temp directory and leaves `<modelsDir>/<id>/` absent,
    // so a retry never sees a permanently "already present" half-bundle.
    // `alreadyPresent` short-circuits on `<modelsDir>/<id>/scene.gltf` existing
    // (or, for the direct-hit fast path, `<modelsDir>/<query>/scene.gltf`);
    // FetchedAsset::maps holds the relative paths written (["scene.gltf",
    // "textures/...", ...]) so its size is the download's own file count.
    std::expected<FetchedAsset, std::string> fetchModel(std::string_view query,
                                                        const std::filesystem::path& modelsDir);

private:
    // Whitelist-checks `url`, issues a GET, and enforces the 64MB body cap;
    // used for both the small JSON endpoints and the (much larger) file
    // downloads, since the transport already buffers the whole body either way.
    std::expected<std::string, std::string> get(const std::string& url) const;

    HttpTransport transport_;
    std::chrono::seconds timeout_;
};

} // namespace kumo::agent
