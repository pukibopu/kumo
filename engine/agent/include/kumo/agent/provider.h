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

protected:
    ILLMProvider() = default;
};

} // namespace kumo::agent
