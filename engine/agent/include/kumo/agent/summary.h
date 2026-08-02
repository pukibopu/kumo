#pragma once

#include <kumo/agent/chat.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kumo::agent {

// Rough token estimate: bytes/4. Coarse by design; the compression threshold is
// a soft budget, not an exact count.
std::size_t approxTokens(const ChatMessage& message);
std::size_t approxTokens(std::span<const ChatMessage> messages);

bool shouldCompress(std::span<const ChatMessage> messages, std::size_t thresholdTokens);

// Index `cutoff` such that [0, cutoff) is summarized and [cutoff, size) is kept
// verbatim. Guarantees: keeps at least keepRecentMessages of the most recent
// messages; NEVER cuts between an assistant message carrying toolCalls and the
// Role::Tool message answering it (the Messages API rejects a dangling
// tool_use); returns 0 when compression is not worthwhile (cutoff would be < 2).
std::size_t compressionCutoff(std::span<const ChatMessage> messages,
                              std::size_t keepRecentMessages);

// Request that asks the model to compress [0, cutoff) into one state summary.
// English prompt, no tools.
ChatRequest makeSummaryRequest(std::span<const ChatMessage> messages, const std::string& model,
                               std::uint32_t maxTokens);

// The single user message that replaces the compressed prefix.
ChatMessage makeSummaryMessage(std::string summaryText);

// The newest reference images (at most maxImages, order kept) plus the newest
// non-empty detail hint in [0, cutoff): re-attached to the summary message so
// compression never discards what later iterations must compare against.
std::pair<std::vector<UserImage>, std::string>
collectReferenceImages(std::span<const ChatMessage> messages, std::size_t maxImages);

} // namespace kumo::agent
