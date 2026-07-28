#include <kumo/agent/summary.h>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

namespace kumo::agent {

std::size_t approxTokens(const ChatMessage& message) {
    std::size_t bytes = message.text.size();
    for (const ToolCall& call : message.toolCalls) {
        bytes += call.argumentsJson.size();
    }
    for (const ToolResult& result : message.toolResults) {
        bytes += result.contentJson.size();
    }
    if (bytes == 0) {
        return 0;
    }
    return std::max<std::size_t>(1, bytes / 4);
}

std::size_t approxTokens(std::span<const ChatMessage> messages) {
    std::size_t total = 0;
    for (const ChatMessage& message : messages) {
        total += approxTokens(message);
    }
    return total;
}

bool shouldCompress(std::span<const ChatMessage> messages, std::size_t thresholdTokens) {
    return approxTokens(messages) >= thresholdTokens;
}

std::size_t compressionCutoff(std::span<const ChatMessage> messages,
                              std::size_t keepRecentMessages) {
    const auto size = static_cast<std::ptrdiff_t>(messages.size());
    std::ptrdiff_t cutoff = size - static_cast<std::ptrdiff_t>(keepRecentMessages);
    if (cutoff < 0) {
        cutoff = 0;
    }
    while (cutoff > 0) {
        const bool answersSummarizedCall =
            cutoff < size && messages[static_cast<std::size_t>(cutoff)].role == Role::Tool;
        const ChatMessage& previous = messages[static_cast<std::size_t>(cutoff - 1)];
        const bool splitsToolUse = previous.role == Role::Assistant && !previous.toolCalls.empty();
        if (!answersSummarizedCall && !splitsToolUse) {
            break;
        }
        --cutoff;
    }
    if (cutoff < 2) {
        return 0;
    }
    return static_cast<std::size_t>(cutoff);
}

namespace {

// Maps tool_use ids to names so tool results (which carry only the id) can be
// rendered with a human-readable tool name in the transcript.
std::string renderTranscript(std::span<const ChatMessage> messages) {
    std::string transcript;
    std::unordered_map<std::string, std::string> callNames;
    for (const ChatMessage& message : messages) {
        switch (message.role) {
        case Role::System:
            if (!message.text.empty()) {
                transcript += std::format("system: {}\n", message.text);
            }
            break;
        case Role::User:
            transcript += std::format("user: {}\n", message.text);
            break;
        case Role::Assistant:
            if (!message.text.empty()) {
                transcript += std::format("assistant: {}\n", message.text);
            }
            for (const ToolCall& call : message.toolCalls) {
                callNames[call.id] = call.name;
                transcript +=
                    std::format("assistant calls tool {} -> {}\n", call.name, call.argumentsJson);
            }
            break;
        case Role::Tool:
            for (const ToolResult& result : message.toolResults) {
                const auto it = callNames.find(result.callId);
                const std::string& name = it != callNames.end() ? it->second : std::string("tool");
                transcript += std::format("tool {} -> {}\n", name, result.contentJson);
            }
            break;
        }
    }
    return transcript;
}

} // namespace

ChatRequest makeSummaryRequest(std::span<const ChatMessage> messages, const std::string& model,
                               std::uint32_t maxTokens) {
    ChatRequest request;
    request.model = model;
    request.systemPrompt = "You compress conversation history.";
    request.maxTokens = maxTokens;
    // `tools` stays default-constructed (empty, non-dangling): this request
    // never needs tool defs.

    ChatMessage prompt;
    prompt.role = Role::User;
    prompt.text = renderTranscript(messages);
    prompt.text += "\nSummarize the conversation above into a compact state summary: what the "
                   "user wants, what was created or changed in the scene (keep exact entity_id "
                   "values, names, positions, colors), and any open follow-ups. Reply with the "
                   "summary only.";
    request.messages = {std::move(prompt)};
    return request;
}

ChatMessage makeSummaryMessage(std::string summaryText) {
    ChatMessage message;
    message.role = Role::User;
    message.text = "Summary of the earlier conversation (compressed): " + summaryText;
    message.stopReason = StopReason::Other;
    return message;
}

} // namespace kumo::agent
