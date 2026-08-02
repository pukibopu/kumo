#include "asset_fetch.h"

#include <kumo/asset/model_resolver.h>
#include <kumo/core/asset_name.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kumo::agent {

namespace {

using nlohmann::json;

constexpr const char* kAssetsTexturesUrl = "https://api.polyhaven.com/assets?t=textures";
constexpr const char* kAssetsHdrisUrl = "https://api.polyhaven.com/assets?t=hdris";
constexpr const char* kAssetsModelsUrl = "https://api.polyhaven.com/assets?t=models";
// Whole body buffered in memory before it is written (the transport already
// does this); this is the safety net against a hostile or misconfigured
// response, not a normal-case limiter (a 2k HDR is ~5-15MB).
constexpr std::size_t kMaxFileBytes = std::size_t{64} * 1024 * 1024;

// Slot names match asset::loadTextureSet's directory convention (albedo/
// normal/roughness/metalness/ao), so a fetched set is immediately consumable
// by material_set_texture with no renaming.
constexpr std::array<std::string_view, 5> kSlotOrder{"albedo", "normal", "roughness", "metalness",
                                                     "ao"};

std::string toLowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Lowercased, whitespace-separated words; the query "Night City" becomes
// {"night", "city"}. Empty on an all-whitespace query.
std::vector<std::string> splitWords(std::string_view query) {
    std::vector<std::string> words;
    std::string word;
    for (char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty()) {
                words.push_back(toLowerCopy(word));
                word.clear();
            }
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) {
        words.push_back(toLowerCopy(word));
    }
    return words;
}

std::vector<std::string> lowerStringArray(const json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }
    out.reserve(value.size());
    for (const auto& item : value) {
        if (item.is_string()) {
            out.push_back(toLowerCopy(item.get<std::string>()));
        }
    }
    return out;
}

bool anyContains(const std::vector<std::string>& lowerItems, std::string_view word) {
    for (const std::string& item : lowerItems) {
        if (item.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Field tiers a query word can hit, highest wins per word.
constexpr int kTierId = 4;
constexpr int kTierName = 3;
constexpr int kTierTag = 2;
constexpr int kTierCategory = 1;
// Dwarfs any possible sum of per-word tiers (<=4 per word), so an exact id
// match always outranks a multi-word partial match.
constexpr int kExactIdBonus = 1000;

struct Candidate {
    std::string id;
    int score = 0;
    int wordsMatched = 0;
};

bool byScoreThenId(const Candidate& a, const Candidate& b) {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    return a.id < b.id;
}

bool byWordsThenScoreThenId(const Candidate& a, const Candidate& b) {
    if (a.wordsMatched != b.wordsMatched) {
        return a.wordsMatched > b.wordsMatched;
    }
    if (a.score != b.score) {
        return a.score > b.score;
    }
    return a.id < b.id;
}

// Up to 3 ids from an already-sorted candidate list, skipping index 0 (the
// winner).
std::vector<std::string> runnersUp(const std::vector<Candidate>& sorted) {
    std::vector<std::string> out;
    for (std::size_t i = 1; i < sorted.size() && out.size() < 3; ++i) {
        out.push_back(sorted[i].id);
    }
    return out;
}

std::optional<std::string> extractUrl(const json& mapNode, std::string_view resolution,
                                      std::string_view format) {
    const auto resIt = mapNode.find(std::string(resolution));
    if (resIt == mapNode.end() || !resIt->is_object()) {
        return std::nullopt;
    }
    const auto fmtIt = resIt->find(std::string(format));
    if (fmtIt == resIt->end() || !fmtIt->is_object()) {
        return std::nullopt;
    }
    const auto urlIt = fmtIt->find("url");
    if (urlIt == fmtIt->end() || !urlIt->is_string()) {
        return std::nullopt;
    }
    return urlIt->get<std::string>();
}

std::vector<std::string> presentTextureMaps(const std::filesystem::path& dir) {
    std::vector<std::string> maps;
    std::error_code ec;
    for (std::string_view slot : kSlotOrder) {
        if (std::filesystem::exists(dir / (std::string(slot) + ".jpg"), ec)) {
            maps.emplace_back(slot);
        }
    }
    return maps;
}

// A model bundle's "include" keys come from Poly Haven's own response, not
// our whitelist-checked query/id; defense in depth against a doctored or
// compromised response steering a write outside the model's own directory
// (mirrors isPlainAssetName's traversal rejection, extended to the '/'
// nesting these relative paths legitimately use, e.g. "textures/foo.jpg").
bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = slash == std::string_view::npos
                                          ? path.substr(start)
                                          : path.substr(start, slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        start = slash + 1;
    }
}

} // namespace

bool isWhitelistedHost(std::string_view url) {
    constexpr std::array<std::string_view, 2> kAllowedPrefixes{"https://api.polyhaven.com/",
                                                               "https://dl.polyhaven.org/"};
    for (std::string_view prefix : kAllowedPrefixes) {
        if (url.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

std::expected<AssetMatch, std::string> pickAsset(std::string_view assetsJson,
                                                 std::string_view query) {
    const json parsed = json::parse(assetsJson, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(std::string("poly haven assets response is not valid JSON"));
    }
    const std::vector<std::string> words = splitWords(query);
    if (words.empty()) {
        return std::unexpected(std::string("query must not be empty"));
    }
    std::string joinedSpace;
    std::string joinedUnderscore;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            joinedSpace += ' ';
            joinedUnderscore += '_';
        }
        joinedSpace += words[i];
        joinedUnderscore += words[i];
    }

    std::vector<Candidate> qualifying;
    std::vector<Candidate> nearMiss;
    for (const auto& item : parsed.items()) {
        const std::string& id = item.key();
        const json& value = item.value();
        if (!value.is_object()) {
            continue;
        }
        const std::string lowerId = toLowerCopy(id);
        std::string lowerName;
        if (const auto nameIt = value.find("name"); nameIt != value.end() && nameIt->is_string()) {
            lowerName = toLowerCopy(nameIt->get<std::string>());
        }
        std::vector<std::string> tags;
        if (const auto it = value.find("tags"); it != value.end()) {
            tags = lowerStringArray(*it);
        }
        std::vector<std::string> categories;
        if (const auto it = value.find("categories"); it != value.end()) {
            categories = lowerStringArray(*it);
        }

        int score = 0;
        int wordsMatched = 0;
        bool allWordsHit = true;
        for (const std::string& word : words) {
            int tier = 0;
            if (lowerId.find(word) != std::string::npos) {
                tier = kTierId;
            } else if (!lowerName.empty() && lowerName.find(word) != std::string::npos) {
                tier = kTierName;
            } else if (anyContains(tags, word)) {
                tier = kTierTag;
            } else if (anyContains(categories, word)) {
                tier = kTierCategory;
            }
            if (tier > 0) {
                score += tier;
                ++wordsMatched;
            } else {
                allWordsHit = false;
            }
        }
        if (lowerId == joinedSpace || lowerId == joinedUnderscore) {
            score += kExactIdBonus;
        }

        if (allWordsHit) {
            qualifying.push_back({id, score, wordsMatched});
        } else if (wordsMatched > 0) {
            nearMiss.push_back({id, score, wordsMatched});
        }
    }

    if (!qualifying.empty()) {
        std::sort(qualifying.begin(), qualifying.end(), byScoreThenId);
        return AssetMatch{.id = qualifying.front().id, .alternatives = runnersUp(qualifying)};
    }

    std::sort(nearMiss.begin(), nearMiss.end(), byWordsThenScoreThenId);
    if (nearMiss.empty()) {
        return std::unexpected(std::format("no asset matches '{}'", query));
    }
    std::string suggestions;
    for (std::size_t i = 0; i < nearMiss.size() && i < 3; ++i) {
        if (i > 0) {
            suggestions += ", ";
        }
        suggestions += nearMiss[i].id;
    }
    return std::unexpected(
        std::format("no asset matches '{}'; closest names: {}", query, suggestions));
}

std::expected<std::unordered_map<std::string, std::string>, std::string>
mapUrls(std::string_view filesJson, AssetKind kind, std::string_view resolution,
        std::string_view format) {
    const json parsed = json::parse(filesJson, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(std::string("poly haven files response is not valid JSON"));
    }

    std::unordered_map<std::string, std::string> out;
    if (kind == AssetKind::Texture) {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kSourceSlots{
            {{"Diffuse", "albedo"},
             {"nor_gl", "normal"},
             {"Rough", "roughness"},
             {"Metal", "metalness"},
             {"AO", "ao"}}};
        for (const auto& [sourceKey, slot] : kSourceSlots) {
            const auto mapIt = parsed.find(std::string(sourceKey));
            if (mapIt == parsed.end() || !mapIt->is_object()) {
                continue;
            }
            if (std::optional<std::string> url = extractUrl(*mapIt, resolution, format)) {
                out.emplace(std::string(slot), std::move(*url));
            }
        }
        if (!out.contains("albedo")) {
            return std::unexpected(
                std::format("this asset has no albedo (Diffuse) map at {} {}", resolution, format));
        }
        return out;
    }

    const auto hdriIt = parsed.find("hdri");
    if (hdriIt == parsed.end() || !hdriIt->is_object()) {
        return std::unexpected(std::string("poly haven response has no hdri maps"));
    }
    if (std::optional<std::string> url = extractUrl(*hdriIt, resolution, format)) {
        out.emplace("hdri", std::move(*url));
        return out;
    }
    return std::unexpected(std::format("no hdri map at {} {}", resolution, format));
}

std::expected<ModelBundle, std::string> modelBundle(std::string_view filesJson,
                                                    std::string_view resolution) {
    const json parsed = json::parse(filesJson, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(std::string("poly haven files response is not valid JSON"));
    }
    const auto gltfIt = parsed.find("gltf");
    if (gltfIt == parsed.end() || !gltfIt->is_object()) {
        return std::unexpected(std::string("this asset has no gltf bundle"));
    }

    // Smallest first: a thumbnail-grade fetch never needs more than 1k, and a
    // smaller bundle is a smaller download either way.
    static constexpr std::array<std::string_view, 4> kFallbackOrder{"1k", "2k", "4k", "8k"};
    auto atResolution = [&](std::string_view res) -> const json* {
        const auto resIt = gltfIt->find(std::string(res));
        if (resIt == gltfIt->end() || !resIt->is_object()) {
            return nullptr;
        }
        const auto fmtIt = resIt->find("gltf");
        if (fmtIt == resIt->end() || !fmtIt->is_object()) {
            return nullptr;
        }
        return &*fmtIt;
    };

    const json* bundle = atResolution(resolution);
    if (bundle == nullptr) {
        for (std::string_view fallback : kFallbackOrder) {
            bundle = atResolution(fallback);
            if (bundle != nullptr) {
                break;
            }
        }
    }
    if (bundle == nullptr) {
        return std::unexpected(std::string("no gltf bundle available at any resolution"));
    }

    const auto urlIt = bundle->find("url");
    if (urlIt == bundle->end() || !urlIt->is_string()) {
        return std::unexpected(std::string("gltf bundle has no url"));
    }
    ModelBundle out;
    out.gltfUrl = urlIt->get<std::string>();
    if (const auto includeIt = bundle->find("include");
        includeIt != bundle->end() && includeIt->is_object()) {
        for (const auto& item : includeIt->items()) {
            if (!item.value().is_object()) {
                continue;
            }
            const auto fileUrlIt = item.value().find("url");
            if (fileUrlIt != item.value().end() && fileUrlIt->is_string()) {
                out.include.emplace(item.key(), fileUrlIt->get<std::string>());
            }
        }
    }
    return out;
}

PolyHavenClient::PolyHavenClient(HttpTransport transport, std::chrono::seconds timeout)
    : transport_(std::move(transport)), timeout_(timeout) {}

std::expected<std::string, std::string> PolyHavenClient::get(const std::string& url) const {
    if (!isWhitelistedHost(url)) {
        return std::unexpected(
            std::format("refusing to fetch from a non-Poly-Haven host: {}", url));
    }
    HttpRequest request;
    request.url = url;
    request.method = "GET";
    // No caller ever aborts a synchronous main-thread tool download; the
    // transport interface still requires a reference to bind (ADR 0005 style
    // shared contract with HttpLLMProvider).
    const std::atomic<bool> abort{false};
    const std::expected<HttpResponse, TransportError> response =
        transport_(request, timeout_, abort);
    if (!response.has_value()) {
        return std::unexpected(
            std::format("network error fetching {}: {}", url, response.error().message));
    }
    if (response->status != 200) {
        return std::unexpected(
            std::format("poly haven returned http {} for {}", response->status, url));
    }
    if (response->body.size() > kMaxFileBytes) {
        return std::unexpected(
            std::format("{} exceeds the 64MB size cap ({} bytes)", url, response->body.size()));
    }
    return response->body;
}

std::expected<FetchedAsset, std::string>
PolyHavenClient::fetchTexture(std::string_view query, const std::filesystem::path& texturesDir) {
    // Fast path, zero network I/O: `query` already names an existing set
    // exactly (the common re-fetch case, since asset_fetch's own result is
    // meant to be reused verbatim as a name).
    if (isPlainAssetName(query)) {
        const std::filesystem::path directHit = texturesDir / std::string(query);
        std::error_code directEc;
        if (std::filesystem::exists(directHit, directEc)) {
            return FetchedAsset{.name = std::string(query),
                                .maps = presentTextureMaps(directHit),
                                .alreadyPresent = true,
                                .alternatives = {}};
        }
    }

    const std::expected<std::string, std::string> assetsBody = get(kAssetsTexturesUrl);
    if (!assetsBody.has_value()) {
        return std::unexpected(assetsBody.error());
    }
    const std::expected<AssetMatch, std::string> picked = pickAsset(*assetsBody, query);
    if (!picked.has_value()) {
        return std::unexpected(picked.error());
    }
    if (!isPlainAssetName(picked->id)) {
        return std::unexpected(
            std::format("poly haven returned an unsafe asset id: {}", picked->id));
    }

    const std::filesystem::path dir = texturesDir / picked->id;
    std::error_code existsEc;
    if (std::filesystem::exists(dir, existsEc)) {
        return FetchedAsset{.name = picked->id,
                            .maps = presentTextureMaps(dir),
                            .alreadyPresent = true,
                            .alternatives = picked->alternatives};
    }

    const std::expected<std::string, std::string> filesBody =
        get(std::format("https://api.polyhaven.com/files/{}", picked->id));
    if (!filesBody.has_value()) {
        return std::unexpected(filesBody.error());
    }
    const std::expected<std::unordered_map<std::string, std::string>, std::string> urls =
        mapUrls(*filesBody, AssetKind::Texture, "1k", "jpg");
    if (!urls.has_value()) {
        return std::unexpected(urls.error());
    }

    // Every map downloaded before anything touches disk: a failure partway
    // through must never leave a half-populated directory behind, since that
    // directory's mere existence is what the idempotency check above trusts.
    std::vector<std::pair<std::string, std::string>> files;
    files.reserve(urls->size());
    for (std::string_view slot : kSlotOrder) {
        const auto it = urls->find(std::string(slot));
        if (it == urls->end()) {
            continue;
        }
        const std::expected<std::string, std::string> bytes = get(it->second);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        files.emplace_back(std::string(slot), *bytes);
    }

    std::error_code mkdirEc;
    std::filesystem::create_directories(dir, mkdirEc);
    std::vector<std::string> maps;
    maps.reserve(files.size());
    for (const auto& [slot, bytes] : files) {
        const std::filesystem::path filePath = dir / (slot + ".jpg");
        std::ofstream out(filePath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return std::unexpected(std::format("failed to write {}", filePath.string()));
        }
        maps.push_back(slot);
    }

    return FetchedAsset{.name = picked->id,
                        .maps = std::move(maps),
                        .alreadyPresent = false,
                        .alternatives = picked->alternatives};
}

std::expected<FetchedAsset, std::string>
PolyHavenClient::fetchEnvironment(std::string_view query, const std::filesystem::path& envDir) {
    // Fast path, zero network I/O: same reasoning as fetchTexture's.
    if (isPlainAssetName(query)) {
        const std::filesystem::path directHit = envDir / (std::string(query) + ".hdr");
        std::error_code directEc;
        if (std::filesystem::exists(directHit, directEc)) {
            return FetchedAsset{
                .name = std::string(query), .maps = {}, .alreadyPresent = true, .alternatives = {}};
        }
    }

    const std::expected<std::string, std::string> assetsBody = get(kAssetsHdrisUrl);
    if (!assetsBody.has_value()) {
        return std::unexpected(assetsBody.error());
    }
    const std::expected<AssetMatch, std::string> picked = pickAsset(*assetsBody, query);
    if (!picked.has_value()) {
        return std::unexpected(picked.error());
    }
    if (!isPlainAssetName(picked->id)) {
        return std::unexpected(
            std::format("poly haven returned an unsafe asset id: {}", picked->id));
    }

    const std::filesystem::path file = envDir / (picked->id + ".hdr");
    std::error_code existsEc;
    if (std::filesystem::exists(file, existsEc)) {
        return FetchedAsset{.name = picked->id,
                            .maps = {},
                            .alreadyPresent = true,
                            .alternatives = picked->alternatives};
    }

    const std::expected<std::string, std::string> filesBody =
        get(std::format("https://api.polyhaven.com/files/{}", picked->id));
    if (!filesBody.has_value()) {
        return std::unexpected(filesBody.error());
    }
    const std::expected<std::unordered_map<std::string, std::string>, std::string> urls =
        mapUrls(*filesBody, AssetKind::Environment, "2k", "hdr");
    if (!urls.has_value()) {
        return std::unexpected(urls.error());
    }
    const auto hdriIt = urls->find("hdri");
    if (hdriIt == urls->end()) {
        return std::unexpected(std::string("no 2k hdri map available"));
    }
    const std::expected<std::string, std::string> bytes = get(hdriIt->second);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }

    std::error_code mkdirEc;
    std::filesystem::create_directories(envDir, mkdirEc);
    std::ofstream out(file, std::ios::binary);
    out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        return std::unexpected(std::format("failed to write {}", file.string()));
    }

    return FetchedAsset{.name = picked->id,
                        .maps = {},
                        .alreadyPresent = false,
                        .alternatives = picked->alternatives};
}

std::expected<FetchedAsset, std::string>
PolyHavenClient::fetchModel(std::string_view query, const std::filesystem::path& modelsDir) {
    // Fast path, zero network I/O: same reasoning as fetchTexture's. The
    // resolver checks every supported layout (<id>.glb, <id>/<id>.gltf,
    // <id>/scene.gltf, each optionally under one category level) — a model
    // present in ANY of them must short-circuit, not just the scene.gltf
    // layout this function itself writes.
    if (isPlainAssetPath(query) && !asset::resolveModelPath(modelsDir, query).empty()) {
        return FetchedAsset{
            .name = std::string(query), .maps = {}, .alreadyPresent = true, .alternatives = {}};
    }

    const std::expected<std::string, std::string> assetsBody = get(kAssetsModelsUrl);
    if (!assetsBody.has_value()) {
        return std::unexpected(assetsBody.error());
    }
    const std::expected<AssetMatch, std::string> picked = pickAsset(*assetsBody, query);
    if (!picked.has_value()) {
        return std::unexpected(picked.error());
    }
    if (!isPlainAssetName(picked->id)) {
        return std::unexpected(
            std::format("poly haven returned an unsafe asset id: {}", picked->id));
    }

    const std::filesystem::path dir = modelsDir / picked->id;
    if (!asset::resolveModelPath(modelsDir, picked->id).empty()) {
        return FetchedAsset{.name = picked->id,
                            .maps = {},
                            .alreadyPresent = true,
                            .alternatives = picked->alternatives};
    }
    // No layout resolved, so an existing `dir` can only hold unrecognized
    // content and the finalization below is guaranteed to reject it — fail
    // now, before the manifest and whole bundle are downloaded synchronously.
    std::error_code earlyDestEc;
    if (std::filesystem::exists(dir, earlyDestEc)) {
        return std::unexpected(
            std::format("destination {} exists but holds no recognized model layout; "
                        "remove it manually and retry",
                        dir.string()));
    }

    const std::expected<std::string, std::string> filesBody =
        get(std::format("https://api.polyhaven.com/files/{}", picked->id));
    if (!filesBody.has_value()) {
        return std::unexpected(filesBody.error());
    }
    const std::expected<ModelBundle, std::string> bundle = modelBundle(*filesBody, "1k");
    if (!bundle.has_value()) {
        return std::unexpected(bundle.error());
    }

    // Downloaded straight into a temp sibling directory and only renamed onto
    // `dir` once the whole bundle is complete: a failure partway through
    // (network or disk) must never leave a partial bundle at `dir` itself,
    // since that directory's mere existence (specifically scene.gltf, per the
    // check above) is what every later fetch of this id trusts as "already
    // present" -- a half-written `dir` would poison it permanently.
    const std::filesystem::path tempDir = modelsDir / (".partial_" + picked->id);
    std::error_code staleEc;
    std::filesystem::remove_all(tempDir, staleEc); // clear a leftover from a prior crashed attempt
    std::error_code mkdirEc;
    std::filesystem::create_directories(tempDir, mkdirEc);

    auto fail = [&](std::string message) -> std::expected<FetchedAsset, std::string> {
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return std::unexpected(std::move(message));
    };

    std::vector<std::string> downloaded;
    downloaded.reserve(bundle->include.size() + 1);

    // Renamed to "scene.gltf" regardless of Poly Haven's own resolution-suffixed
    // filename (e.g. "Barrel_01_1k.gltf"), so the resolver's plain <name>/
    // scene.gltf rule finds it without needing to know that filename. Written
    // first here, but that ordering is now irrelevant to correctness: nothing
    // is visible at `dir` until the rename below.
    const std::expected<std::string, std::string> gltfBytes = get(bundle->gltfUrl);
    if (!gltfBytes.has_value()) {
        return fail(gltfBytes.error());
    }
    const std::filesystem::path gltfPath = tempDir / "scene.gltf";
    {
        std::ofstream out(gltfPath, std::ios::binary);
        out.write(gltfBytes->data(), static_cast<std::streamsize>(gltfBytes->size()));
        if (!out) {
            return fail(std::format("failed to write {}", gltfPath.string()));
        }
    }
    downloaded.emplace_back("scene.gltf");

    for (const auto& [relPath, url] : bundle->include) {
        if (!isSafeRelativePath(relPath)) {
            return fail(std::format("poly haven returned an unsafe include path: {}", relPath));
        }
        const std::expected<std::string, std::string> bytes = get(url);
        if (!bytes.has_value()) {
            return fail(bytes.error());
        }
        const std::filesystem::path filePath = tempDir / relPath;
        std::filesystem::create_directories(filePath.parent_path(), mkdirEc);
        std::ofstream out(filePath, std::ios::binary);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!out) {
            return fail(std::format("failed to write {}", filePath.string()));
        }
        downloaded.push_back(relPath);
    }

    // Reaching here means no supported layout resolved for this id, so an
    // existing `dir` can only hold unrecognized content — never delete it on
    // the fetcher's own authority; surface it instead.
    std::error_code destEc;
    if (std::filesystem::exists(dir, destEc)) {
        return fail(std::format("destination {} exists but holds no recognized model layout; "
                                "remove it manually and retry",
                                dir.string()));
    }
    std::error_code renameEc;
    std::filesystem::rename(tempDir, dir, renameEc);
    if (renameEc) {
        return fail(std::format("failed to finalize model directory {}: {}", dir.string(),
                                renameEc.message()));
    }

    return FetchedAsset{.name = picked->id,
                        .maps = std::move(downloaded),
                        .alreadyPresent = false,
                        .alternatives = picked->alternatives};
}

} // namespace kumo::agent
