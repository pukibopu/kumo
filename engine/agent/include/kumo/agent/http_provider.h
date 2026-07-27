#pragma once

#include <kumo/agent/chat.h>
#include <kumo/agent/provider.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kumo::agent {

struct HttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

struct TransportError {
    enum class Kind { Timeout, ConnectionFailed, Cancelled };
    Kind kind = Kind::ConnectionFailed;
    std::string message;
};

// Synchronous POST; implementations honor `timeout` and poll `abort` so a
// shutting-down session is never stuck behind a slow endpoint.
using HttpTransport = std::function<std::expected<HttpResponse, TransportError>(
    const HttpRequest& request, std::chrono::seconds timeout, const std::atomic<bool>& abort)>;

// NSURLSession-backed transport (ObjC++ shim per ADR 0005/0034), defined in
// http_transport_apple.mm. The shim carries no policy: retry, backoff and error
// classification all live in HttpLLMProvider where they are testable.
HttpTransport makeUrlSessionTransport();

// Template-method base for HTTP providers: encode once, then a retry loop with
// exponential backoff (1s -> 4s with jitter) over 429/5xx/transport failures,
// two retries max; other 4xx fail immediately (ADR 0030). Sleep and jitter are
// injected so tests replay the schedule without real waiting.
class HttpLLMProvider : public ILLMProvider {
public:
    struct Options {
        std::chrono::seconds requestTimeout{120};
        int maxRetries = 2;
        // Invoked from the worker thread before each backoff sleep with
        // (nextAttempt, maxRetries); UI surfaces the retry notice from it.
        std::function<void(int, int)> onRetry;
        // Test seams; null selects the abort-aware default sleep / uniform jitter.
        std::function<void(std::chrono::milliseconds)> sleep;
        std::function<double()> random01;
    };

    CompleteResult complete(const ChatRequest& request) final;
    void abort() noexcept override;

protected:
    HttpLLMProvider(HttpTransport transport, Options options);

    virtual HttpRequest encodeRequest(const ChatRequest& request) const = 0;
    virtual std::expected<ChatMessage, std::string> decodeResponse(std::string_view body) const = 0;

private:
    void backoffSleep(int completedAttempt);

    HttpTransport transport_;
    Options options_;
    std::atomic<bool> abort_{false};
    std::mt19937 rng_{std::random_device{}()};
};

// Chat Completions against any OpenAI-compatible endpoint (Ollama, LM Studio,
// llama.cpp, or the real thing); `apiKey` may be empty for local servers, which
// do not check it (ADR 0012).
class OpenAiProvider final : public HttpLLMProvider {
public:
    OpenAiProvider(std::string baseUrl, std::string apiKey, HttpTransport transport,
                   Options options = {});

protected:
    HttpRequest encodeRequest(const ChatRequest& request) const override;
    std::expected<ChatMessage, std::string> decodeResponse(std::string_view body) const override;

private:
    std::string baseUrl_;
    std::string apiKey_;
};

// Anthropic Messages API (official endpoint or a compatible relay, ADR 0012).
class ClaudeProvider final : public HttpLLMProvider {
public:
    ClaudeProvider(std::string baseUrl, std::string apiKey, HttpTransport transport,
                   Options options = {});

protected:
    HttpRequest encodeRequest(const ChatRequest& request) const override;
    std::expected<ChatMessage, std::string> decodeResponse(std::string_view body) const override;

private:
    std::string baseUrl_;
    std::string apiKey_;
};

} // namespace kumo::agent
