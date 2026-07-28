#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace kumo::agent {

// Blocks the agent worker on a destructive tool until the user decides, while the
// main thread keeps rendering and polls pending() each frame to draw the dialog
// (ADR 0022). The composition root only creates one when confirm_destructive is
// enabled; a null gate means destructive tools run unprompted. One gate is shared
// across sessions, so concurrent askers (scene + shader) each queue their own
// prompt instead of clobbering each other.
//
// Prompts carry a monotonic id and queue in arrival order; the UI answers one at
// a time from the front. A decision is keyed by that id, so it can only ever
// land on the asker that owns it — a stray double-click cannot answer whichever
// prompt is showing next. The gate holds no latched terminal state, so it is
// safely shared across session lifetimes.
class ConfirmationGate {
public:
    struct Prompt {
        std::uint64_t id = 0;
        std::string toolName;
        std::string argumentsJson;
    };

    // Worker thread: blocks until resolve() targets this prompt or `abort`
    // becomes true; false means denied or aborted.
    bool ask(std::string toolName, std::string argumentsJson, const std::atomic<bool>& abort);

    // Main thread: the prompt at the front of the queue, empty when there is none.
    std::optional<Prompt> pending() const;
    // Ignored unless `promptId` names a prompt still in the queue.
    void resolve(std::uint64_t promptId, bool approved);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Prompt> pending_;
    std::unordered_map<std::uint64_t, bool> decisions_;
    std::uint64_t lastPromptId_ = 0;
};

} // namespace kumo::agent
