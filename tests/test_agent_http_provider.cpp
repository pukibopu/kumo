#include <doctest/doctest.h>

#include <kumo/agent/http_provider.h>
#include <kumo/core/file.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace kumo;
using namespace kumo::agent;
using nlohmann::json;

namespace {

std::string fixtureText(const char* provider, const char* name) {
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / provider / name;
    const auto text = readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

// Scripted transport: replays results in order and records every request.
struct TransportScript {
    std::vector<std::expected<HttpResponse, TransportError>> results;
    std::size_t next = 0;
    std::vector<HttpRequest> requests;

    HttpTransport fn() {
        return [this](const HttpRequest& request, std::chrono::seconds,
                      const std::atomic<bool>&) -> std::expected<HttpResponse, TransportError> {
            requests.push_back(request);
            if (next >= results.size()) {
                return std::unexpected(TransportError{
                    .kind = TransportError::Kind::ConnectionFailed, .message = "script over"});
            }
            return results[next++];
        };
    }
};

// Minimal concrete provider: the retry loop is what these tests pin, not codecs.
class TestProvider final : public HttpLLMProvider {
public:
    TestProvider(HttpTransport transport, Options options)
        : HttpLLMProvider(std::move(transport), std::move(options)) {}

protected:
    HttpRequest encodeRequest(const ChatRequest&) const override {
        return {.url = "http://test/v1", .headers = {}, .body = "request"};
    }
    std::expected<ChatMessage, std::string> decodeResponse(std::string_view body) const override {
        if (body == "bad") {
            return std::unexpected("undecodable body");
        }
        ChatMessage message;
        message.role = Role::Assistant;
        message.text = body;
        message.stopReason = StopReason::EndTurn;
        return message;
    }
};

struct RetryHarness {
    TransportScript script;
    std::vector<std::chrono::milliseconds> sleeps;
    std::vector<std::pair<int, int>> retryNotices;
    double jitter = 0.5;

    TestProvider make() {
        HttpLLMProvider::Options options;
        options.sleep = [this](std::chrono::milliseconds delay) { sleeps.push_back(delay); };
        options.random01 = [this] { return jitter; };
        options.onRetry = [this](int attempt, int max) { retryNotices.push_back({attempt, max}); };
        return TestProvider(script.fn(), std::move(options));
    }
};

ChatRequest emptyRequest() {
    ChatRequest request;
    request.model = "m";
    return request;
}

} // namespace

TEST_CASE("HttpLLMProvider retries 429 with the documented backoff schedule") {
    RetryHarness harness;
    harness.script.results = {
        HttpResponse{429, fixtureText("openai", "error_429.json")},
        HttpResponse{429, fixtureText("openai", "error_429.json")},
        HttpResponse{200, "hello"},
    };
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(result.has_value());
    CHECK(result->text == "hello");
    CHECK(harness.script.requests.size() == 3);
    // jitter 0.5 -> exactly the 1s and 4s bases (ADR 0030).
    REQUIRE(harness.sleeps.size() == 2);
    CHECK(harness.sleeps[0].count() == 1000);
    CHECK(harness.sleeps[1].count() == 4000);
    REQUIRE(harness.retryNotices.size() == 2);
    CHECK(harness.retryNotices[0] == std::pair{1, 2});
    CHECK(harness.retryNotices[1] == std::pair{2, 2});
}

TEST_CASE("HttpLLMProvider backoff jitter stays within +-20 percent") {
    RetryHarness harness;
    harness.jitter = 0.0;
    harness.script.results = {
        HttpResponse{500, "{}"},
        HttpResponse{500, "{}"},
        HttpResponse{200, "ok"},
    };
    TestProvider provider = harness.make();
    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(harness.sleeps.size() == 2);
    CHECK(harness.sleeps[0].count() == 800);
    CHECK(harness.sleeps[1].count() == 3200);
}

TEST_CASE("HttpLLMProvider fails 401 immediately with the endpoint's message") {
    RetryHarness harness;
    harness.script.results = {HttpResponse{401, fixtureText("openai", "error_401.json")}};
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().kind == ProviderError::Kind::Http);
    CHECK(result.error().httpStatus == 401);
    CHECK(result.error().retriesUsed == 0);
    CHECK(result.error().message.find("Incorrect API key") != std::string::npos);
    CHECK(harness.script.requests.size() == 1);
    CHECK(harness.sleeps.empty());
}

TEST_CASE("HttpLLMProvider gives up after two retries on persistent 5xx") {
    RetryHarness harness;
    harness.script.results = {
        HttpResponse{503, "{}"},
        HttpResponse{503, "{}"},
        HttpResponse{503, "{}"},
    };
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().httpStatus == 503);
    CHECK(result.error().retriesUsed == 2);
    CHECK(harness.script.requests.size() == 3);
}

TEST_CASE("HttpLLMProvider retries transport failures and reports Network on defeat") {
    RetryHarness harness;
    harness.script.results = {
        std::unexpected(
            TransportError{.kind = TransportError::Kind::Timeout, .message = "timed out"}),
        HttpResponse{200, "recovered"},
    };
    TestProvider provider = harness.make();
    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(result.has_value());
    CHECK(result->text == "recovered");

    RetryHarness dead;
    dead.script.results = {
        std::unexpected(TransportError{.kind = TransportError::Kind::ConnectionFailed,
                                       .message = "connection refused"}),
        std::unexpected(TransportError{.kind = TransportError::Kind::ConnectionFailed,
                                       .message = "connection refused"}),
        std::unexpected(TransportError{.kind = TransportError::Kind::ConnectionFailed,
                                       .message = "connection refused"}),
    };
    TestProvider deadProvider = dead.make();
    const CompleteResult failure = deadProvider.complete(emptyRequest());
    REQUIRE(!failure.has_value());
    CHECK(failure.error().kind == ProviderError::Kind::Network);
    CHECK(failure.error().retriesUsed == 2);
}

TEST_CASE("HttpLLMProvider treats cancellation and abort as terminal") {
    RetryHarness harness;
    harness.script.results = {std::unexpected(
        TransportError{.kind = TransportError::Kind::Cancelled, .message = "cancelled"})};
    TestProvider provider = harness.make();
    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().kind == ProviderError::Kind::Cancelled);
    CHECK(harness.sleeps.empty());

    RetryHarness aborted;
    TestProvider abortedProvider = aborted.make();
    abortedProvider.abort();
    const CompleteResult abortedResult = abortedProvider.complete(emptyRequest());
    REQUIRE(!abortedResult.has_value());
    CHECK(abortedResult.error().kind == ProviderError::Kind::Cancelled);
    CHECK(aborted.script.requests.empty());
}

TEST_CASE("HttpLLMProvider reports undecodable success bodies as Decode errors") {
    RetryHarness harness;
    harness.script.results = {HttpResponse{200, "bad"}};
    TestProvider provider = harness.make();
    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().kind == ProviderError::Kind::Decode);
    // Decode failures are deterministic; retrying cannot help.
    CHECK(harness.script.requests.size() == 1);
}

TEST_CASE("OpenAiProvider shapes the request for a local key-less endpoint") {
    TransportScript script;
    script.results = {
        HttpResponse{200, R"({"choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})"}};
    OpenAiProvider provider("http://127.0.0.1:11434/", "", script.fn(), {});

    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(script.requests.size() == 1);
    const HttpRequest& request = script.requests[0];
    CHECK(request.url == "http://127.0.0.1:11434/v1/chat/completions");
    bool hasAuthorization = false;
    for (const auto& [name, value] : request.headers) {
        hasAuthorization = hasAuthorization || name == "Authorization";
    }
    CHECK(!hasAuthorization);
    CHECK(!json::parse(request.body, nullptr, false).is_discarded());
}

TEST_CASE("ClaudeProvider sends the Anthropic headers") {
    TransportScript script;
    script.results = {
        HttpResponse{200, R"({"content":[{"type":"text","text":"hi"}],"stop_reason":"end_turn"})"}};
    ClaudeProvider provider("https://api.anthropic.com", "sk-test", script.fn(), {});

    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(script.requests.size() == 1);
    const HttpRequest& request = script.requests[0];
    CHECK(request.url == "https://api.anthropic.com/v1/messages");
    std::string apiKey;
    std::string version;
    for (const auto& [name, value] : request.headers) {
        if (name == "x-api-key") {
            apiKey = value;
        } else if (name == "anthropic-version") {
            version = value;
        }
    }
    CHECK(apiKey == "sk-test");
    CHECK(!version.empty());
}
