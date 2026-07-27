#include <kumo/agent/http_provider.h>

#include <kumo/agent/claude_codec.h>
#include <kumo/agent/openai_codec.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <thread>

namespace kumo::agent {

namespace {

using nlohmann::json;

// Both OpenAI and Anthropic wrap failures as {"error":{"message":...}}; fall
// back to the (truncated) raw body for anything else.
std::string extractErrorMessage(const std::string& body) {
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_object()) {
        const auto error = parsed.find("error");
        if (error != parsed.end() && error->is_object()) {
            const auto message = error->find("message");
            if (message != error->end() && message->is_string()) {
                return message->get<std::string>();
            }
        }
        if (error != parsed.end() && error->is_string()) {
            return error->get<std::string>();
        }
    }
    return body.substr(0, std::min<std::size_t>(body.size(), 200));
}

std::string stripTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

ProviderError cancelledError(int retriesUsed) {
    return {.kind = ProviderError::Kind::Cancelled,
            .httpStatus = 0,
            .message = "cancelled",
            .retriesUsed = retriesUsed};
}

} // namespace

HttpLLMProvider::HttpLLMProvider(HttpTransport transport, Options options)
    : transport_(std::move(transport)), options_(std::move(options)) {}

void HttpLLMProvider::abort() noexcept {
    abort_.store(true);
}

void HttpLLMProvider::backoffSleep(int completedAttempt) {
    // 1s then 4s base, +-20% jitter (ADR 0030).
    const double base = completedAttempt == 0 ? 1000.0 : 4000.0;
    const double jitter = options_.random01
                              ? options_.random01()
                              : std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
    const auto delay = std::chrono::milliseconds(
        static_cast<long long>(base * (0.8 + 0.4 * std::clamp(jitter, 0.0, 1.0))));
    if (options_.sleep) {
        options_.sleep(delay);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline && !abort_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

CompleteResult HttpLLMProvider::complete(const ChatRequest& request) {
    HttpRequest encoded;
    try {
        encoded = encodeRequest(request);
    } catch (const std::exception& e) {
        // Dependency exceptions stop at the module boundary (ADR 0035).
        return std::unexpected(ProviderError{
            .kind = ProviderError::Kind::Decode, .httpStatus = 0, .message = e.what()});
    }

    ProviderError lastError;
    for (int attempt = 0; attempt <= options_.maxRetries; ++attempt) {
        if (abort_.load()) {
            return std::unexpected(cancelledError(attempt));
        }
        const auto response = transport_(encoded, options_.requestTimeout, abort_);
        if (!response.has_value()) {
            if (response.error().kind == TransportError::Kind::Cancelled) {
                return std::unexpected(cancelledError(attempt));
            }
            // Connection timeouts and resets are transient by assumption.
            lastError = {.kind = ProviderError::Kind::Network,
                         .httpStatus = 0,
                         .message = response.error().message,
                         .retriesUsed = attempt};
        } else if (response->status == 200) {
            std::expected<ChatMessage, std::string> decoded;
            try {
                decoded = decodeResponse(response->body);
            } catch (const std::exception& e) {
                decoded = std::unexpected(std::string(e.what()));
            }
            if (!decoded.has_value()) {
                return std::unexpected(ProviderError{.kind = ProviderError::Kind::Decode,
                                                     .httpStatus = response->status,
                                                     .message = decoded.error(),
                                                     .retriesUsed = attempt});
            }
            return *decoded;
        } else if (response->status == 429 || response->status >= 500) {
            lastError = {.kind = ProviderError::Kind::Http,
                         .httpStatus = response->status,
                         .message = extractErrorMessage(response->body),
                         .retriesUsed = attempt};
        } else {
            // Client errors (401 bad key, 400 bad request) never resolve on
            // retry; fail fast so the user checks the configuration (ADR 0030).
            return std::unexpected(ProviderError{.kind = ProviderError::Kind::Http,
                                                 .httpStatus = response->status,
                                                 .message = extractErrorMessage(response->body),
                                                 .retriesUsed = attempt});
        }

        if (attempt == options_.maxRetries) {
            break;
        }
        if (options_.onRetry) {
            options_.onRetry(attempt + 1, options_.maxRetries);
        }
        backoffSleep(attempt);
    }
    return std::unexpected(lastError);
}

OpenAiProvider::OpenAiProvider(std::string baseUrl, std::string apiKey, HttpTransport transport,
                               Options options)
    : HttpLLMProvider(std::move(transport), std::move(options)),
      baseUrl_(stripTrailingSlash(std::move(baseUrl))), apiKey_(std::move(apiKey)) {}

HttpRequest OpenAiProvider::encodeRequest(const ChatRequest& request) const {
    HttpRequest out;
    out.url = baseUrl_ + "/v1/chat/completions";
    out.headers.push_back({"Content-Type", "application/json"});
    if (!apiKey_.empty()) {
        out.headers.push_back({"Authorization", "Bearer " + apiKey_});
    }
    out.body = encodeChatCompletionsRequest(request);
    return out;
}

std::expected<ChatMessage, std::string>
OpenAiProvider::decodeResponse(std::string_view body) const {
    return decodeChatCompletionsResponse(body);
}

ClaudeProvider::ClaudeProvider(std::string baseUrl, std::string apiKey, HttpTransport transport,
                               Options options)
    : HttpLLMProvider(std::move(transport), std::move(options)),
      baseUrl_(stripTrailingSlash(std::move(baseUrl))), apiKey_(std::move(apiKey)) {}

HttpRequest ClaudeProvider::encodeRequest(const ChatRequest& request) const {
    HttpRequest out;
    out.url = baseUrl_ + "/v1/messages";
    out.headers.push_back({"Content-Type", "application/json"});
    out.headers.push_back({"anthropic-version", "2023-06-01"});
    if (!apiKey_.empty()) {
        out.headers.push_back({"x-api-key", apiKey_});
    }
    out.body = encodeMessagesRequest(request);
    return out;
}

std::expected<ChatMessage, std::string>
ClaudeProvider::decodeResponse(std::string_view body) const {
    return decodeMessagesResponse(body);
}

} // namespace kumo::agent
