#pragma once

#include <kumo/agent/chat.h>

namespace kumo::agent {

// One blocking round trip to a model. Implementations run on the agent worker
// thread and absorb transport concerns themselves (timeout, retry, backoff — see
// ADR 0030), so callers only see a message or a ProviderError.
class ILLMProvider {
public:
    virtual ~ILLMProvider() = default;

    ILLMProvider(const ILLMProvider&) = delete;
    ILLMProvider& operator=(const ILLMProvider&) = delete;

    virtual CompleteResult complete(const ChatRequest& request) = 0;

    // Asks an in-flight complete() to return early with Kind::Cancelled; safe
    // from any thread. Providers without long waits keep the no-op default.
    virtual void abort() noexcept {}

protected:
    ILLMProvider() = default;
};

} // namespace kumo::agent
