#include <doctest/doctest.h>

// Private kumo_agent header (engine/agent/src), not a public interface;
// reached into here to unit-test detail::parseRetryAfterSeconds directly
// (ADR 0030 update), the same way test_renderer_compat.cpp reaches into
// kumo_renderer's shader_load.h.
#include "retry_after.h"

#include <kumo/agent/http_provider.h>
#include <kumo/core/file.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
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
        HttpResponse{429, fixtureText("openai", "error_429.json"), std::nullopt},
        HttpResponse{429, fixtureText("openai", "error_429.json"), std::nullopt},
        HttpResponse{200, "hello", std::nullopt},
    };
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(result.has_value());
    CHECK(result->text == "hello");
    CHECK(harness.script.requests.size() == 3);
    // jitter 0.5 -> exactly the 1s and 4s bases (ADR 0030); no Retry-After
    // header in this fixture, so the blind backoff schedule applies.
    REQUIRE(harness.sleeps.size() == 2);
    CHECK(harness.sleeps[0].count() == 1000);
    CHECK(harness.sleeps[1].count() == 4000);
    // Denominator is the 429 budget (3), not the 5xx/network one (2).
    REQUIRE(harness.retryNotices.size() == 2);
    CHECK(harness.retryNotices[0] == std::pair{1, 3});
    CHECK(harness.retryNotices[1] == std::pair{2, 3});
}

TEST_CASE("HttpLLMProvider allows a third retry on persistent 429 (one more than 5xx)") {
    RetryHarness harness;
    harness.script.results = {
        HttpResponse{429, "{}", std::nullopt},
        HttpResponse{429, "{}", std::nullopt},
        HttpResponse{429, "{}", std::nullopt},
        HttpResponse{429, "{}", std::nullopt},
    };
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().httpStatus == 429);
    CHECK(result.error().retriesUsed == 3);
    CHECK(harness.script.requests.size() == 4);
}

TEST_CASE("HttpLLMProvider honors a delta-seconds Retry-After header over the blind backoff") {
    RetryHarness harness;
    harness.jitter = 0.5; // envelope midpoint -> exactly 1.0x, no distortion.
    harness.script.results = {
        HttpResponse{429, "{}", "2.5"},
        HttpResponse{200, "recovered", std::nullopt},
    };
    TestProvider provider = harness.make();

    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(result.has_value());
    REQUIRE(harness.sleeps.size() == 1);
    // 2.5s, not the blind-backoff 1s first tier.
    CHECK(harness.sleeps[0].count() == 2500);
}

TEST_CASE("HttpLLMProvider caps a Retry-After wait at 30 seconds") {
    RetryHarness harness;
    harness.jitter = 0.5;
    harness.script.results = {
        HttpResponse{429, "{}", "120"},
        HttpResponse{200, "recovered", std::nullopt},
    };
    TestProvider provider = harness.make();

    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(harness.sleeps.size() == 1);
    CHECK(harness.sleeps[0].count() == 30000);
}

TEST_CASE("HttpLLMProvider falls back to backoff when Retry-After is an HTTP-date or garbage") {
    RetryHarness harness;
    harness.jitter = 0.5;
    harness.script.results = {
        HttpResponse{429, "{}", "Wed, 21 Oct 2015 07:28:00 GMT"},
        HttpResponse{200, "recovered", std::nullopt},
    };
    TestProvider provider = harness.make();

    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(harness.sleeps.size() == 1);
    CHECK(harness.sleeps[0].count() == 1000);
}

TEST_CASE("HttpLLMProvider backoff jitter stays within +-20 percent") {
    RetryHarness harness;
    harness.jitter = 0.0;
    harness.script.results = {
        HttpResponse{500, "{}", std::nullopt},
        HttpResponse{500, "{}", std::nullopt},
        HttpResponse{200, "ok", std::nullopt},
    };
    TestProvider provider = harness.make();
    REQUIRE(provider.complete(emptyRequest()).has_value());
    REQUIRE(harness.sleeps.size() == 2);
    CHECK(harness.sleeps[0].count() == 800);
    CHECK(harness.sleeps[1].count() == 3200);
}

TEST_CASE("HttpLLMProvider fails 401 immediately with the endpoint's message") {
    RetryHarness harness;
    harness.script.results = {
        HttpResponse{401, fixtureText("openai", "error_401.json"), std::nullopt}};
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
        HttpResponse{503, "{}", std::nullopt},
        HttpResponse{503, "{}", std::nullopt},
        HttpResponse{503, "{}", std::nullopt},
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
        HttpResponse{200, "recovered", std::nullopt},
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
    harness.script.results = {HttpResponse{200, "bad", std::nullopt}};
    TestProvider provider = harness.make();
    const CompleteResult result = provider.complete(emptyRequest());
    REQUIRE(!result.has_value());
    CHECK(result.error().kind == ProviderError::Kind::Decode);
    // Decode failures are deterministic; retrying cannot help.
    CHECK(harness.script.requests.size() == 1);
}

TEST_CASE("OpenAiProvider shapes the request for a local key-less endpoint") {
    TransportScript script;
    script.results = {HttpResponse{
        200, R"({"choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})", std::nullopt}};
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
        HttpResponse{200, R"({"content":[{"type":"text","text":"hi"}],"stop_reason":"end_turn"})",
                     std::nullopt}};
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

TEST_CASE("parseRetryAfterSeconds accepts the delta-seconds form") {
    const auto integer = detail::parseRetryAfterSeconds("7");
    REQUIRE(integer.has_value());
    CHECK(*integer == 7.0);

    const auto fractional = detail::parseRetryAfterSeconds("2.5");
    REQUIRE(fractional.has_value());
    CHECK(*fractional == 2.5);

    // Header values commonly arrive without surrounding whitespace, but
    // tolerate it anyway.
    const auto padded = detail::parseRetryAfterSeconds("  3  ");
    REQUIRE(padded.has_value());
    CHECK(*padded == 3.0);

    CHECK(*detail::parseRetryAfterSeconds("0") == 0.0);
}

TEST_CASE("parseRetryAfterSeconds rejects garbage and the HTTP-date form") {
    CHECK(!detail::parseRetryAfterSeconds("").has_value());
    CHECK(!detail::parseRetryAfterSeconds("not-a-number").has_value());
    CHECK(!detail::parseRetryAfterSeconds("Wed, 21 Oct 2015 07:28:00 GMT").has_value());
    // Trailing garbage after a valid-looking prefix must not partially parse.
    CHECK(!detail::parseRetryAfterSeconds("7 seconds").has_value());
}

TEST_CASE("parseRetryAfterSeconds rejects negative values") {
    CHECK(!detail::parseRetryAfterSeconds("-1").has_value());
    CHECK(!detail::parseRetryAfterSeconds("-0.5").has_value());
}

TEST_CASE("parseRetryAfterSeconds rejects an out-of-range huge value") {
    // Far beyond what a double can represent; from_chars reports
    // result_out_of_range rather than silently saturating.
    CHECK(!detail::parseRetryAfterSeconds("1" + std::string(400, '0')).has_value());
    // A large but perfectly finite value is still accepted; capping to the 30s
    // ceiling is the retry loop's job, not the parser's.
    const auto large = detail::parseRetryAfterSeconds("1000000");
    REQUIRE(large.has_value());
    CHECK(*large == 1000000.0);
}
