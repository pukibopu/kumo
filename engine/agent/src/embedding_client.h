#pragma once

#include <kumo/agent/http_provider.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::agent {

// Requests per batch against /v1/embeddings; OpenAI accepts far more, but 64
// keeps request bodies small enough for local OpenAI-compatible servers.
inline constexpr std::size_t kEmbeddingBatchSize = 64;

// Pure, no I/O: the /v1/embeddings request body for one batch.
std::string encodeEmbeddingRequest(std::string_view model, std::span<const std::string> texts);

// Pure, no I/O: `data[i].embedding` rows reordered by their `index` field, so
// the result lines up with the request's input order. Error on malformed
// JSON, an API error object, a count mismatch or inconsistent dimensions.
std::expected<std::vector<std::vector<float>>, std::string>
parseEmbeddingResponse(std::string_view body, std::size_t expectedCount);

// OpenAI-compatible embeddings endpoint (MR): same injected-transport pattern
// as PolyHavenClient/HttpLLMProvider, so tests replay fixtures. No retry
// policy: the index builder treats any failure as "skip embeddings this run"
// and a query-time failure degrades that one search to FTS.
class EmbeddingClient {
public:
    EmbeddingClient(HttpTransport transport, std::string baseUrl, std::string apiKey,
                    std::string model, std::chrono::seconds timeout = std::chrono::seconds{60});

    // Embeds `texts` in kEmbeddingBatchSize batches, one POST each; all rows
    // share one dimension or the whole call errors.
    std::expected<std::vector<std::vector<float>>, std::string>
    embed(std::span<const std::string> texts);

    const std::string& model() const { return model_; }

private:
    HttpTransport transport_;
    std::string baseUrl_;
    std::string apiKey_;
    std::string model_;
    std::chrono::seconds timeout_;
};

} // namespace kumo::agent
