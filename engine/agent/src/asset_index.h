#pragma once

#include <kumo/math/math.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::agent {

// A pre-scanned summary of the asset library (MA milestone), written by
// `viewer --thumbnails`/`--index` to <assetDir>/index.json and read by
// asset_list (so it can answer without a directory scan) and the retrieval
// tools asset_search/material_recipe_search (MR). Generated, not
// hand-authored: parsing is lenient by design (see parseAssetIndex).
enum class AssetIndexKind { Texture, Model, Environment, Recipe, Spec };

// One row of index.json. Every kind-specific field stays at its default
// (nullopt / empty) for kinds it does not apply to, e.g. an environment
// entry's `dimensions`.
struct AssetIndexEntry {
    // asset_list identity: a texture-set/env stem, or a model name (which may
    // carry one category prefix, e.g. "props/crate" -- kumo::isPlainAssetPath).
    std::string id;
    AssetIndexKind kind = AssetIndexKind::Texture;
    std::string name;     // display name; asset_list falls back to `id` when empty
    std::string category; // path- or pack.json-derived; empty when ungrouped
    std::string style;    // "realistic" | "stylized"; empty means unset
    std::string license;
    std::string source;
    std::vector<std::string> tags;
    std::string caption;   // filled by a later milestone (MR); empty until then
    std::string thumbnail; // relative to assetDir, e.g. ".thumbnails/models/props_crate.png"

    std::vector<std::string> maps;           // textures: present map slots
    std::optional<std::uint32_t> resolution; // textures: max side in px, across present maps
    std::optional<math::float3> dimensions;  // models: world-space AABB size
    std::optional<std::uint64_t> triangles;  // models: summed index count / 3
    std::optional<bool> instancingOk;        // models: triangles below the instancing budget

    std::int64_t embeddingOffset = -1; // -1: no embedding yet (all kinds; MR fills this in)
};

struct AssetIndexEmbedding {
    std::string model;
    std::uint32_t dim = 0;
    std::string file; // relative to assetDir
};

struct AssetIndex {
    int version = 1;
    std::string generated; // opaque to readers; a timestamp string
    std::optional<AssetIndexEmbedding> embedding;
    std::vector<AssetIndexEntry> entries;
};

// Pure, no I/O.
std::string serializeAssetIndex(const AssetIndex& index);

// Pure, no I/O. Lenient, since index.json is a generated cache rather than a
// hand-authored file whose shape is worth enforcing strictly: malformed JSON
// or a non-object top level returns nullopt; an entry missing `id` or with an
// unrecognized `kind` string is skipped rather than failing the whole parse;
// every other field (including the whole top-level `embedding` block) is
// optional and defaults per AssetIndexEntry/AssetIndex's own defaults.
std::optional<AssetIndex> parseAssetIndex(std::string_view json);

// Reads <assetDir>/index.json. nullopt on a missing file or anything
// parseAssetIndex rejects -- both mean "fall back to a directory scan" to
// callers like asset_list.
std::optional<AssetIndex> loadAssetIndex(const std::filesystem::path& assetDir);

// Writes <assetDir>/index.json atomically (temp file + rename), so a reader
// racing the builder never sees a half-written index. False on any failure.
bool saveAssetIndex(const std::filesystem::path& assetDir, const AssetIndex& index);

// The embedding sidecar (MR): contiguous float32 rows of `embedding.dim`
// floats each; an entry's embeddingOffset is its row number. Load returns
// nullopt on a missing/unreadable file, a zero dim, or a byte size that is
// not a whole number of rows -- callers then degrade to FTS-only search.
std::optional<std::vector<float>> loadEmbeddingRows(const std::filesystem::path& assetDir,
                                                    const AssetIndexEmbedding& embedding);

// Atomic like saveAssetIndex. `rows` must be a whole number of dim-sized rows.
bool saveEmbeddingRows(const std::filesystem::path& assetDir, const AssetIndexEmbedding& embedding,
                       std::span<const float> rows);

} // namespace kumo::agent
