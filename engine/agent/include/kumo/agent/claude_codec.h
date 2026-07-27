#pragma once

#include <kumo/agent/chat.h>

#include <expected>
#include <string>
#include <string_view>

namespace kumo::agent {

// Anthropic Messages API codec. Free functions so fixtures can pin the wire
// format without any transport (ADR 0031); until a real key is available this
// path is fixture-verified only (ADR 0043).

// Serializes the request body: system goes in the top-level field, Role::Tool
// messages become role:"user" tool_result blocks, adjacent same-role messages
// are merged defensively (the API rejects consecutive same-role messages), and
// parametersSchema strings are inlined as input_schema objects.
std::string encodeMessagesRequest(const ChatRequest& request);

// Decodes text and tool_use content blocks; stop_reason maps end_turn ->
// EndTurn, tool_use -> ToolUse, max_tokens -> MaxTokens, anything else -> Other.
std::expected<ChatMessage, std::string> decodeMessagesResponse(std::string_view body);

} // namespace kumo::agent
