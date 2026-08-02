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

private:
    // Whitelist-checks `url`, issues a GET, and enforces the 64MB body cap;
    // used for both the small JSON endpoints and the (much larger) file
    // downloads, since the transport already buffers the whole body either way.
    std::expected<std::string, std::string> get(const std::string& url) const;

    HttpTransport transport_;
    std::chrono::seconds timeout_;
};

} // namespace kumo::agent
