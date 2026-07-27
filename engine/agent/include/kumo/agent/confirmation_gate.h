#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace kumo::agent {

// Blocks the agent worker on a destructive tool until the user decides, while the
// main thread keeps rendering and polls pending() each frame to draw the dialog
// (ADR 0022). The composition root only creates one when confirm_destructive is
// enabled; a null gate means destructive tools run unprompted.
class ConfirmationGate {
public:
    struct Prompt {
        std::string toolName;
        std::string argumentsJson;
    };

    // Worker thread: blocks until resolve() or cancelAll(); false means denied.
    bool ask(Prompt prompt);

    // Main thread: the prompt awaiting a decision, empty when there is none.
    std::optional<Prompt> pending() const;
    void resolve(bool approved);

    // Unblocks a waiting worker with denial; later ask() calls fail immediately.
    void cancelAll();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Prompt> prompt_;
    bool resolved_ = false;
    bool approved_ = false;
    bool cancelled_ = false;
};

} // namespace kumo::agent
