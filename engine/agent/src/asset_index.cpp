#include "asset_index.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace kumo::agent {

namespace {

using nlohmann::json;

std::string kindToString(AssetIndexKind kind) {
    switch (kind) {
    case AssetIndexKind::Texture:
        return "texture";
    case AssetIndexKind::Model:
        return "model";
    case AssetIndexKind::Environment:
        return "env";
    case AssetIndexKind::Recipe:
        return "recipe";
    case AssetIndexKind::Spec:
        return "spec";
    }
    return "texture";
}

std::optional<AssetIndexKind> kindFromString(const std::string& text) {
    if (text == "texture") {
        return AssetIndexKind::Texture;
    }
    if (text == "model") {
        return AssetIndexKind::Model;
    }
    if (text == "env") {
        return AssetIndexKind::Environment;
    }
    if (text == "recipe") {
        return AssetIndexKind::Recipe;
    }
    if (text == "spec") {
        return AssetIndexKind::Spec;
    }
    return std::nullopt;
}

// Temp-sibling write + rename, so readers never observe a partial file.
bool writeFileAtomic(const std::filesystem::path& path, const char* data, std::size_t size) {
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(data, static_cast<std::streamsize>(size));
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::string readStringField(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> readStringArray(const json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

json entryToJson(const AssetIndexEntry& e) {
    json j{{"id", e.id}, {"kind", kindToString(e.kind)}};
    if (!e.name.empty()) {
        j["name"] = e.name;
    }
    if (!e.category.empty()) {
        j["category"] = e.category;
    }
    if (!e.style.empty()) {
        j["style"] = e.style;
    }
    if (!e.license.empty()) {
        j["license"] = e.license;
    }
    if (!e.source.empty()) {
        j["source"] = e.source;
    }
    if (!e.tags.empty()) {
        j["tags"] = e.tags;
    }
    if (!e.caption.empty()) {
        j["caption"] = e.caption;
    }
    if (!e.thumbnail.empty()) {
        j["thumbnail"] = e.thumbnail;
    }
    if (!e.maps.empty()) {
        j["maps"] = e.maps;
    }
    if (e.resolution.has_value()) {
        j["resolution"] = *e.resolution;
    }
    if (e.dimensions.has_value()) {
        j["dimensions"] = json::array({e.dimensions->x, e.dimensions->y, e.dimensions->z});
    }
    if (e.triangles.has_value()) {
        j["triangles"] = *e.triangles;
    }
    if (e.instancingOk.has_value()) {
        j["instancing_ok"] = *e.instancingOk;
    }
    if (e.embeddingOffset != -1) {
        j["embedding_offset"] = e.embeddingOffset;
    }
    return j;
}

// nullopt only when the entry is unusable outright (no id, or an
// unrecognized kind); every other field defaults per AssetIndexEntry's own
// defaults when absent or of an unexpected type, per parseAssetIndex's
// leniency contract.
std::optional<AssetIndexEntry> entryFromJson(const json& obj) {
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const std::string id = readStringField(obj, "id");
    if (id.empty()) {
        return std::nullopt;
    }
    const std::optional<AssetIndexKind> kind = kindFromString(readStringField(obj, "kind"));
    if (!kind.has_value()) {
        return std::nullopt;
    }

    AssetIndexEntry e;
    e.id = id;
    e.kind = *kind;
    e.name = readStringField(obj, "name");
    e.category = readStringField(obj, "category");
    e.style = readStringField(obj, "style");
    e.license = readStringField(obj, "license");
    e.source = readStringField(obj, "source");
    e.tags = readStringArray(obj, "tags");
    e.caption = readStringField(obj, "caption");
    e.thumbnail = readStringField(obj, "thumbnail");
    e.maps = readStringArray(obj, "maps");
    if (const auto it = obj.find("resolution"); it != obj.end() && it->is_number()) {
        e.resolution = it->get<std::uint32_t>();
    }
    if (const auto it = obj.find("dimensions"); it != obj.end() && it->is_array() &&
                                                it->size() == 3 && (*it)[0].is_number() &&
                                                (*it)[1].is_number() && (*it)[2].is_number()) {
        e.dimensions =
            math::float3{(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    }
    if (const auto it = obj.find("triangles"); it != obj.end() && it->is_number()) {
        e.triangles = it->get<std::uint64_t>();
    }
    if (const auto it = obj.find("instancing_ok"); it != obj.end() && it->is_boolean()) {
        e.instancingOk = it->get<bool>();
    }
    if (const auto it = obj.find("embedding_offset"); it != obj.end() && it->is_number()) {
        e.embeddingOffset = it->get<std::int64_t>();
    }
    return e;
}

} // namespace

std::string serializeAssetIndex(const AssetIndex& index) {
    json root{{"version", index.version}, {"generated", index.generated}};
    if (index.embedding.has_value()) {
        root["embedding"] = json{{"model", index.embedding->model},
                                 {"dim", index.embedding->dim},
                                 {"file", index.embedding->file}};
    }
    json entries = json::array();
    for (const AssetIndexEntry& e : index.entries) {
        entries.push_back(entryToJson(e));
    }
    root["entries"] = std::move(entries);
    return root.dump(2);
}

std::optional<AssetIndex> parseAssetIndex(std::string_view text) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    AssetIndex index;
    if (const auto it = parsed.find("version"); it != parsed.end() && it->is_number()) {
        index.version = it->get<int>();
    }
    index.generated = readStringField(parsed, "generated");
    if (const auto it = parsed.find("embedding"); it != parsed.end() && it->is_object()) {
        AssetIndexEmbedding embedding;
        embedding.model = readStringField(*it, "model");
        if (const auto dimIt = it->find("dim"); dimIt != it->end() && dimIt->is_number()) {
            embedding.dim = dimIt->get<std::uint32_t>();
        }
        embedding.file = readStringField(*it, "file");
        index.embedding = std::move(embedding);
    }
    if (const auto it = parsed.find("entries"); it != parsed.end() && it->is_array()) {
        index.entries.reserve(it->size());
        for (const auto& item : *it) {
            if (std::optional<AssetIndexEntry> entry = entryFromJson(item)) {
                index.entries.push_back(std::move(*entry));
            }
        }
    }
    return index;
}

std::optional<AssetIndex> loadAssetIndex(const std::filesystem::path& assetDir) {
    std::ifstream in(assetDir / "index.json", std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parseAssetIndex(buffer.str());
}

bool saveAssetIndex(const std::filesystem::path& assetDir, const AssetIndex& index) {
    std::error_code ec;
    std::filesystem::create_directories(assetDir, ec);
    const std::string text = serializeAssetIndex(index);
    return writeFileAtomic(assetDir / "index.json", text.data(), text.size());
}

std::optional<std::vector<float>> loadEmbeddingRows(const std::filesystem::path& assetDir,
                                                    const AssetIndexEmbedding& embedding) {
    if (embedding.dim == 0 || embedding.file.empty()) {
        return std::nullopt;
    }
    std::ifstream in(assetDir / embedding.file, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    const std::size_t rowBytes = static_cast<std::size_t>(embedding.dim) * sizeof(float);
    if (size < 0 || static_cast<std::size_t>(size) % rowBytes != 0) {
        return std::nullopt;
    }
    std::vector<float> rows(static_cast<std::size_t>(size) / sizeof(float));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(rows.data()), size);
    if (!in) {
        return std::nullopt;
    }
    return rows;
}

bool saveEmbeddingRows(const std::filesystem::path& assetDir, const AssetIndexEmbedding& embedding,
                       std::span<const float> rows) {
    if (embedding.dim == 0 || embedding.file.empty() || rows.size() % embedding.dim != 0) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(assetDir, ec);
    return writeFileAtomic(assetDir / embedding.file, reinterpret_cast<const char*>(rows.data()),
                           rows.size() * sizeof(float));
}

} // namespace kumo::agent
