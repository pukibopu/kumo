#pragma once

#include <kumo/agent/provider.h>

#include <cstddef>
#include <string>
#include <vector>

namespace kumo::agent {

// Replays a scripted transcript for tests and the viewer's --offline mode: each
// complete() call returns the next scripted message regardless of the request.
// Requests are recorded with the tools span deep-copied, so they stay readable
// after the registry is gone. Not thread-safe on its own; callers observe
// recorded state only while the owning session is idle.
class FakeProvider final : public ILLMProvider {
public:
    struct Recorded {
        std::string model;
        std::string systemPrompt;
        std::vector<ChatMessage> messages;
        std::vector<ToolDef> tools;
        std::uint32_t maxTokens = 0;
    };

    explicit FakeProvider(std::vector<ChatMessage> script,
                          std::string exhaustedText = "offline script exhausted");

    CompleteResult complete(const ChatRequest& request) override;

    const std::vector<Recorded>& requests() const { return requests_; }
    bool exhausted() const { return next_ >= script_.size(); }

private:
    std::vector<ChatMessage> script_;
    std::string exhaustedText_;
    std::size_t next_ = 0;
    std::vector<Recorded> requests_;
};

} // namespace kumo::agent
