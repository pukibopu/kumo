#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace kumo::agent {

// Blocks the agent worker on a destructive tool until the user decides, while the
// main thread keeps rendering and polls pending() each frame to draw the dialog
// (ADR 0022). The composition root only creates one when confirm_destructive is
// enabled; a null gate means destructive tools run unprompted.
//
// Prompts carry a monotonic id so a decision can never land on a prompt the user
// has not seen (two destructive calls in one turn reopen the dialog in place; a
// stray double-click must not answer the second one). The gate holds no latched
// terminal state, so it is safely shared across session lifetimes.
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

    // Main thread: the prompt awaiting a decision, empty when there is none.
    std::optional<Prompt> pending() const;
    // Ignored unless `promptId` matches the prompt currently waiting.
    void resolve(std::uint64_t promptId, bool approved);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Prompt> prompt_;
    std::uint64_t lastPromptId_ = 0;
    bool resolved_ = false;
    bool approved_ = false;
};

} // namespace kumo::agent
