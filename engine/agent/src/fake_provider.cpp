#include <kumo/agent/fake_provider.h>

#include <utility>

namespace kumo::agent {

FakeProvider::FakeProvider(std::vector<ChatMessage> script, std::string exhaustedText)
    : script_(std::move(script)), exhaustedText_(std::move(exhaustedText)) {}

CompleteResult FakeProvider::complete(const ChatRequest& request) {
    requests_.push_back({.model = request.model,
                         .systemPrompt = request.systemPrompt,
                         .messages = request.messages,
                         .tools = {request.tools.begin(), request.tools.end()},
                         .maxTokens = request.maxTokens});
    if (next_ >= script_.size()) {
        ChatMessage message;
        message.role = Role::Assistant;
        message.text = exhaustedText_;
        message.stopReason = StopReason::EndTurn;
        return message;
    }
    return script_[next_++];
}

} // namespace kumo::agent
