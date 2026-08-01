#pragma once

#include <kumo/agent/tool_registry.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace kumo::agent {

enum class Role { System, User, Assistant, Tool };

enum class StopReason { EndTurn, ToolUse, MaxTokens, Other };

struct ToolCall {
    std::string id;
    std::string name;
    std::string argumentsJson;
};

struct ToolResult {
    std::string callId;
    std::string contentJson;
    bool isError = false;
    // Empty when the result carries no attachment; filled by the session when the
    // tool's result JSON carries an image_path.
    std::string imagePngBase64;
};

// Provider-neutral message: deliberately the intersection of the Anthropic and
// OpenAI wire formats (ADR 0005). A Role::Tool message carries `toolResults` and no
// text; an assistant message that wants tools carries `toolCalls`.
struct ChatMessage {
    Role role = Role::User;
    std::string text;
    std::vector<ToolCall> toolCalls;
    std::vector<ToolResult> toolResults;
    StopReason stopReason = StopReason::Other;
};

// `tools` is a non-owning view, normally ToolRegistry::defs(); the registry must
// outlive the request.
struct ChatRequest {
    std::string model;
    std::string systemPrompt;
    std::vector<ChatMessage> messages;
    std::span<const ToolDef> tools;
    std::uint32_t maxTokens = 4096;
    // OpenAI reasoning models: sent as reasoning_effort when non-empty. Newer
    // models reject function tools on chat completions unless this is "none";
    // the anthropic codec ignores it.
    std::string reasoningEffort;
};

struct ProviderError {
    enum class Kind { Config, Network, Http, Decode, Cancelled };

    Kind kind = Kind::Config;
    int httpStatus = 0;
    std::string message;
    int retriesUsed = 0;
};

using CompleteResult = std::expected<ChatMessage, ProviderError>;

} // namespace kumo::agent
