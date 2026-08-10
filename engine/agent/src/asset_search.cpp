#include "asset_search.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kumo::agent::search {

namespace {

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool equalsInsensitive(std::string_view a, std::string_view b) {
    return a.size() == b.size() && toLower(a) == toLower(b);
}

bool containsToken(std::string_view haystack, const std::string& lowerToken) {
    return !haystack.empty() && toLower(haystack).find(lowerToken) != std::string::npos;
}

bool isWordChar(unsigned char c) {
    return std::isalnum(c) != 0 || c >= 0x80;
}

} // namespace

bool passesFilters(const AssetIndexEntry& entry, const Filters& filters) {
    if (!filters.kinds.empty() &&
        std::find(filters.kinds.begin(), filters.kinds.end(), entry.kind) == filters.kinds.end()) {
        return false;
    }
    if (!filters.category.empty() && !equalsInsensitive(entry.category, filters.category)) {
        return false;
    }
    if (!filters.style.empty() && !equalsInsensitive(entry.style, filters.style)) {
        return false;
    }
    if (filters.maxDimension.has_value() && entry.dimensions.has_value()) {
        const float longest =
            std::max({entry.dimensions->x, entry.dimensions->y, entry.dimensions->z});
        if (longest > *filters.maxDimension) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> tokenizeQuery(std::string_view query) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : query) {
        if (isWordChar(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

float ftsScore(const AssetIndexEntry& entry, std::span<const std::string> tokens) {
    if (tokens.empty()) {
        return 0.0f;
    }
    int total = 0;
    for (const std::string& token : tokens) {
        int best = 0;
        if (containsToken(entry.id, token) || containsToken(entry.name, token)) {
            best = 4;
        } else if (std::any_of(entry.tags.begin(), entry.tags.end(),
                               [&](const std::string& tag) { return containsToken(tag, token); })) {
            best = 3;
        } else if (containsToken(entry.caption, token)) {
            best = 2;
        } else if (containsToken(entry.category, token)) {
            best = 1;
        }
        total += best;
    }
    return static_cast<float>(total) / (static_cast<float>(tokens.size()) * 4.0f);
}

float cosine(std::span<const float> a, std::span<const float> b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }
    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    if (normA <= 0.0f || normB <= 0.0f) {
        return 0.0f;
    }
    return dot / (std::sqrt(normA) * std::sqrt(normB));
}

std::vector<Match> searchIndex(const AssetIndex& index, std::span<const float> rows,
                               const Filters& filters, std::span<const std::string> tokens,
                               std::span<const float> queryVec, int limit) {
    limit = std::clamp(limit, 1, kMaxResults);
    const std::size_t dim = queryVec.size();
    const bool wantsScore = !tokens.empty() || !queryVec.empty();

    struct Candidate {
        std::size_t entryIndex;
        float fts;
        float cos;
        bool hasCos;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        const AssetIndexEntry& entry = index.entries[i];
        if (!passesFilters(entry, filters)) {
            continue;
        }
        Candidate c{.entryIndex = i, .fts = ftsScore(entry, tokens), .cos = 0.0f, .hasCos = false};
        if (dim > 0 && entry.embeddingOffset >= 0) {
            const std::size_t begin = static_cast<std::size_t>(entry.embeddingOffset) * dim;
            if (begin + dim <= rows.size()) {
                c.cos = cosine(rows.subspan(begin, dim), queryVec);
                c.hasCos = true;
            }
        }
        candidates.push_back(c);
    }

    std::vector<Match> matches;
    if (!wantsScore) {
        for (const Candidate& c : candidates) {
            matches.push_back({.entryIndex = c.entryIndex, .score = 0.0f});
        }
    } else {
        // RRF: each ranking contributes 1/(k + rank); a candidate must appear
        // in at least one to survive. The FTS list holds only actual hits,
        // the cosine list every candidate with an embedding row.
        auto rankAndScore = [&](auto metric, auto member, std::vector<float>& scores) {
            std::vector<std::size_t> order;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (member(candidates[i])) {
                    order.push_back(i);
                }
            }
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                if (metric(candidates[a]) != metric(candidates[b])) {
                    return metric(candidates[a]) > metric(candidates[b]);
                }
                return index.entries[candidates[a].entryIndex].id <
                       index.entries[candidates[b].entryIndex].id;
            });
            for (std::size_t rank = 0; rank < order.size(); ++rank) {
                scores[order[rank]] += 1.0f / (static_cast<float>(kRrfK + rank) + 1.0f);
            }
        };
        std::vector<float> scores(candidates.size(), 0.0f);
        rankAndScore([](const Candidate& c) { return c.fts; },
                     [](const Candidate& c) { return c.fts > 0.0f; }, scores);
        rankAndScore([](const Candidate& c) { return c.cos; },
                     [](const Candidate& c) { return c.hasCos; }, scores);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (scores[i] > 0.0f) {
                matches.push_back({.entryIndex = candidates[i].entryIndex, .score = scores[i]});
            }
        }
    }

    std::sort(matches.begin(), matches.end(), [&](const Match& a, const Match& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return index.entries[a.entryIndex].id < index.entries[b.entryIndex].id;
    });
    if (matches.size() > static_cast<std::size_t>(limit)) {
        matches.resize(static_cast<std::size_t>(limit));
    }
    return matches;
}

void refreshSearchCacheLocked(SearchCache& cache, const std::filesystem::path& assetDir) {
    std::error_code ec;
    const std::filesystem::file_time_type mtime =
        std::filesystem::last_write_time(assetDir / "index.json", ec);
    if (ec) {
        cache.loaded = true;
        cache.index.reset();
        cache.rows.clear();
        return;
    }
    if (cache.loaded && cache.index.has_value() && mtime == cache.indexTime) {
        return;
    }
    cache.loaded = true;
    cache.indexTime = mtime;
    cache.index = loadAssetIndex(assetDir);
    cache.rows.clear();
    if (cache.index.has_value() && cache.index->embedding.has_value()) {
        if (std::optional<std::vector<float>> rows =
                loadEmbeddingRows(assetDir, *cache.index->embedding)) {
            cache.rows = std::move(*rows);
        }
    }
}

} // namespace kumo::agent::search
