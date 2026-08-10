#pragma once

#include "asset_index.h"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::agent::search {

// Result caps (MR): candidates are capped so a retrieval tool can attach
// thumbnails without flooding the transcript.
inline constexpr int kMaxResults = 5;
inline constexpr int kDefaultResults = 3;
// Reciprocal Rank Fusion constant (the standard k=60): 1/(k + rank) flattens
// the head enough that one ranker's top pick cannot drown out the other.
inline constexpr int kRrfK = 60;

// Hard pre-filters, applied before any scoring. Empty = no constraint.
struct Filters {
    std::vector<AssetIndexKind> kinds; // whitelist; empty admits every kind
    std::string category;              // exact, case-insensitive
    std::string style;                 // exact, case-insensitive
    // Longest AABB axis must fit; only meaningful for model entries (entries
    // without dimensions pass, since the constraint cannot be evaluated).
    std::optional<float> maxDimension;
};

bool passesFilters(const AssetIndexEntry& entry, const Filters& filters);

// Lowercased words split on ASCII non-alphanumerics; bytes >= 0x80 count as
// word characters so a CJK query survives as one token instead of vanishing.
std::vector<std::string> tokenizeQuery(std::string_view query);

// Per token the best field tier it hits as a case-insensitive substring:
// id/name 4 > tags 3 > caption 2 > category 1; summed over tokens and
// normalized by tokens.size()*4, so the score stays in [0,1]. 0 when
// `tokens` is empty or nothing matches.
float ftsScore(const AssetIndexEntry& entry, std::span<const std::string> tokens);

// 0 on length mismatch or a zero-norm side.
float cosine(std::span<const float> a, std::span<const float> b);

struct Match {
    std::size_t entryIndex = 0; // into AssetIndex::entries
    float score = 0.0f;         // RRF-fused; ~0.033 for a double rank-1 hit
};

// The retrieval pipeline over one loaded index: hard filters, then
// Reciprocal Rank Fusion over two rankings -- the FTS hits (ftsScore > 0)
// and cosine over every candidate with a valid embeddingOffset into `rows`
// (needs a dim matching `queryVec`). A candidate absent from both rankings
// is dropped; with neither tokens nor a query vector, filtered entries come
// back in id order (browse mode). Ties break on entry id and `limit` is
// clamped to [1, kMaxResults], so results are deterministic.
std::vector<Match> searchIndex(const AssetIndex& index, std::span<const float> rows,
                               const Filters& filters, std::span<const std::string> tokens,
                               std::span<const float> queryVec, int limit);

// One asset library's lazily loaded index + embedding sidecar, shared across
// the scene and shader tool registries the way SceneToolContext::groups is;
// mtime-keyed so a `viewer --index` rebuild is picked up without a restart.
struct SearchCache {
    std::mutex mutex;
    bool loaded = false;
    std::filesystem::file_time_type indexTime{};
    std::optional<AssetIndex> index; // nullopt: no readable index.json
    std::vector<float> rows;         // empty when the index has no sidecar
};

// (Re)loads when index.json's mtime changed or nothing was loaded yet; call
// with `cache.mutex` held and keep holding it while reading index/rows.
void refreshSearchCacheLocked(SearchCache& cache, const std::filesystem::path& assetDir);

} // namespace kumo::agent::search
