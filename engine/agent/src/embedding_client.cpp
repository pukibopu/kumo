#include "embedding_client.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <format>
#include <utility>

namespace kumo::agent {

namespace {

using nlohmann::json;

std::string stripTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

} // namespace

std::string encodeEmbeddingRequest(std::string_view model, std::span<const std::string> texts) {
    json input = json::array();
    for (const std::string& text : texts) {
        input.push_back(text);
    }
    return json{{"model", model}, {"input", std::move(input)}}.dump();
}

std::expected<std::vector<std::vector<float>>, std::string>
parseEmbeddingResponse(std::string_view body, std::size_t expectedCount) {
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected("embeddings response is not valid JSON");
    }
    if (const auto errorIt = parsed.find("error"); errorIt != parsed.end()) {
        const std::string message = errorIt->is_object() && errorIt->contains("message") &&
                                            (*errorIt)["message"].is_string()
                                        ? (*errorIt)["message"].get<std::string>()
                                        : errorIt->dump();
        return std::unexpected(std::format("embeddings API error: {}", message));
    }
    const auto dataIt = parsed.find("data");
    if (dataIt == parsed.end() || !dataIt->is_array()) {
        return std::unexpected("embeddings response has no data array");
    }
    if (dataIt->size() != expectedCount) {
        return std::unexpected(std::format("embeddings response has {} rows, expected {}",
                                           dataIt->size(), expectedCount));
    }
    std::vector<std::vector<float>> rows(expectedCount);
    std::vector<bool> filled(expectedCount, false);
    for (const json& item : *dataIt) {
        if (!item.is_object() || !item.contains("index") || !item["index"].is_number_unsigned() ||
            !item.contains("embedding") || !item["embedding"].is_array()) {
            return std::unexpected("embeddings response row is malformed");
        }
        const std::size_t index = item["index"].get<std::size_t>();
        if (index >= expectedCount || filled[index]) {
            return std::unexpected("embeddings response row index is out of range or duplicated");
        }
        std::vector<float>& row = rows[index];
        row.reserve(item["embedding"].size());
        for (const json& value : item["embedding"]) {
            if (!value.is_number()) {
                return std::unexpected("embeddings response row holds a non-number");
            }
            row.push_back(value.get<float>());
        }
        filled[index] = true;
    }
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() != rows[0].size()) {
            return std::unexpected("embeddings response rows disagree on dimension");
        }
    }
    return rows;
}

EmbeddingClient::EmbeddingClient(HttpTransport transport, std::string baseUrl, std::string apiKey,
                                 std::string model, std::chrono::seconds timeout)
    : transport_(std::move(transport)), baseUrl_(stripTrailingSlash(std::move(baseUrl))),
      apiKey_(std::move(apiKey)), model_(std::move(model)), timeout_(timeout) {}

std::expected<std::vector<std::vector<float>>, std::string>
EmbeddingClient::embed(std::span<const std::string> texts) {
    std::vector<std::vector<float>> all;
    all.reserve(texts.size());
    static const std::atomic<bool> kNeverAbort{false};
    for (std::size_t begin = 0; begin < texts.size(); begin += kEmbeddingBatchSize) {
        const std::span<const std::string> batch =
            texts.subspan(begin, std::min(kEmbeddingBatchSize, texts.size() - begin));
        HttpRequest request;
        request.url = baseUrl_ + "/v1/embeddings";
        request.headers.push_back({"Content-Type", "application/json"});
        if (!apiKey_.empty()) {
            request.headers.push_back({"Authorization", "Bearer " + apiKey_});
        }
        request.body = encodeEmbeddingRequest(model_, batch);
        const auto response = transport_(request, timeout_, kNeverAbort);
        if (!response.has_value()) {
            return std::unexpected(
                std::format("embeddings request failed: {}", response.error().message));
        }
        if (response->status != 200) {
            auto parsed = parseEmbeddingResponse(response->body, batch.size());
            return std::unexpected(parsed.has_value()
                                       ? std::format("embeddings HTTP {}", response->status)
                                       : parsed.error());
        }
        auto rows = parseEmbeddingResponse(response->body, batch.size());
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        if (!all.empty() && !rows->empty() && (*rows)[0].size() != all[0].size()) {
            return std::unexpected("embeddings batches disagree on dimension");
        }
        for (std::vector<float>& row : *rows) {
            all.push_back(std::move(row));
        }
    }
    return all;
}

} // namespace kumo::agent
