#pragma once

#include <kumo/agent/chat.h>

#include <expected>
#include <string>
#include <string_view>

namespace kumo::agent {

// OpenAI Chat Completions codec (ADR 0012: the de-facto standard local endpoints
// speak — Ollama, LM Studio, llama.cpp). Free functions so fixtures can pin the
// wire format without any transport (ADR 0031).

// Serializes the request body; parametersSchema strings are inlined as JSON
// objects. Never fails: the registry guarantees parseable schemas.
std::string encodeChatCompletionsRequest(const ChatRequest& request);

// Decodes choices[0]; finish_reason maps stop -> EndTurn, tool_calls -> ToolUse,
// length -> MaxTokens, anything else -> Other. The error string is diagnostic
// text for ProviderError, not model-facing payload.
std::expected<ChatMessage, std::string> decodeChatCompletionsResponse(std::string_view body);

} // namespace kumo::agent
