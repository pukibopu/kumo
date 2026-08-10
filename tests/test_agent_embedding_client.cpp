#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), same pattern as
// test_agent_asset_fetch.cpp.
#include "embedding_client.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace kumo;
using namespace kumo::agent;
using nlohmann::json;

namespace {

// Fixture transport in the test_agent_asset_fetch.cpp mold: records requests,
// replays canned responses.
struct FakeTransport {
    std::vector<HttpRequest> requests;
    std::vector<std::expected<HttpResponse, TransportError>> responses;

    HttpTransport fn() {
        return [this](const HttpRequest& request, std::chrono::seconds,
                      const std::atomic<bool>&) -> std::expected<HttpResponse, TransportError> {
            requests.push_back(request);
            if (requests.size() > responses.size()) {
                return HttpResponse{.status = 500, .body = "unexpected extra request"};
            }
            return responses[requests.size() - 1];
        };
    }
};

std::string embeddingBody(std::size_t count, std::size_t dim, bool shuffled = false) {
    json data = json::array();
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index = shuffled ? count - 1 - i : i;
        json row = json::array();
        for (std::size_t d = 0; d < dim; ++d) {
            row.push_back(static_cast<double>(index) + static_cast<double>(d) * 0.1);
        }
        data.push_back(json{{"index", index}, {"embedding", std::move(row)}});
    }
    return json{{"data", std::move(data)}}.dump();
}

} // namespace

TEST_CASE("encodeEmbeddingRequest carries the model and every input in order") {
    const std::vector<std::string> texts{"wet asphalt", "night sky"};
    const json body = json::parse(encodeEmbeddingRequest("test-model", texts));
    CHECK(body["model"] == "test-model");
    REQUIRE(body["input"].size() == 2);
    CHECK(body["input"][0] == "wet asphalt");
    CHECK(body["input"][1] == "night sky");
}

TEST_CASE("parseEmbeddingResponse reorders rows by their index field") {
    const auto rows = parseEmbeddingResponse(embeddingBody(3, 2, /*shuffled=*/true), 3);
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 3);
    CHECK((*rows)[0][0] == doctest::Approx(0.0f));
    CHECK((*rows)[1][0] == doctest::Approx(1.0f));
    CHECK((*rows)[2][0] == doctest::Approx(2.0f));
    CHECK((*rows)[2][1] == doctest::Approx(2.1f));
}

TEST_CASE("parseEmbeddingResponse rejects malformed shapes") {
    CHECK(!parseEmbeddingResponse("not json", 1).has_value());
    CHECK(!parseEmbeddingResponse(R"({"data": "nope"})", 1).has_value());
    CHECK(!parseEmbeddingResponse(embeddingBody(2, 4), 3).has_value()); // count mismatch
    // API error object surfaces its message.
    const auto apiError = parseEmbeddingResponse(R"({"error": {"message": "model not found"}})", 1);
    REQUIRE(!apiError.has_value());
    CHECK(apiError.error().find("model not found") != std::string::npos);
    // Duplicated index.
    const auto dup = parseEmbeddingResponse(
        R"({"data": [{"index": 0, "embedding": [1]}, {"index": 0, "embedding": [2]}]})", 2);
    CHECK(!dup.has_value());
    // Rows disagreeing on dimension.
    const auto ragged = parseEmbeddingResponse(
        R"({"data": [{"index": 0, "embedding": [1, 2]}, {"index": 1, "embedding": [3]}]})", 2);
    CHECK(!ragged.has_value());
}

TEST_CASE("EmbeddingClient batches 64 inputs per request against /v1/embeddings") {
    FakeTransport transport;
    transport.responses.push_back(HttpResponse{.status = 200, .body = embeddingBody(64, 2)});
    transport.responses.push_back(HttpResponse{.status = 200, .body = embeddingBody(64, 2)});
    transport.responses.push_back(HttpResponse{.status = 200, .body = embeddingBody(2, 2)});
    EmbeddingClient client(transport.fn(), "http://127.0.0.1:11434/", "key", "test-model");

    std::vector<std::string> texts(130, "text");
    const auto rows = client.embed(texts);
    REQUIRE(rows.has_value());
    CHECK(rows->size() == 130);
    REQUIRE(transport.requests.size() == 3);
    // Trailing slash stripped, one endpoint for all batches.
    CHECK(transport.requests[0].url == "http://127.0.0.1:11434/v1/embeddings");
    CHECK(json::parse(transport.requests[0].body)["input"].size() == 64);
    CHECK(json::parse(transport.requests[2].body)["input"].size() == 2);
    bool sawAuth = false;
    for (const auto& [name, value] : transport.requests[0].headers) {
        sawAuth = sawAuth || (name == "Authorization" && value == "Bearer key");
    }
    CHECK(sawAuth);
}

TEST_CASE("EmbeddingClient surfaces transport and HTTP failures") {
    FakeTransport transport;
    transport.responses.push_back(std::unexpected(TransportError{
        .kind = TransportError::Kind::ConnectionFailed, .message = "connection refused"}));
    EmbeddingClient down(transport.fn(), "http://127.0.0.1:9", "", "m");
    const std::vector<std::string> one{"text"};
    const auto refused = down.embed(one);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().find("connection refused") != std::string::npos);

    FakeTransport transport429;
    transport429.responses.push_back(
        HttpResponse{.status = 429, .body = R"({"error": {"message": "rate limited"}})"});
    EmbeddingClient limited(transport429.fn(), "http://127.0.0.1:9", "", "m");
    const auto rateLimited = limited.embed(one);
    REQUIRE(!rateLimited.has_value());
    CHECK(rateLimited.error().find("rate limited") != std::string::npos);
}

TEST_CASE("EmbeddingClient rejects batches that disagree on dimension") {
    FakeTransport transport;
    transport.responses.push_back(HttpResponse{.status = 200, .body = embeddingBody(64, 2)});
    transport.responses.push_back(HttpResponse{.status = 200, .body = embeddingBody(1, 3)});
    EmbeddingClient client(transport.fn(), "http://x", "", "m");
    const std::vector<std::string> texts(65, "text");
    const auto rows = client.embed(texts);
    REQUIRE(!rows.has_value());
    CHECK(rows.error().find("dimension") != std::string::npos);
}
