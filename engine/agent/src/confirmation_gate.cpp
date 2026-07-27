#include <kumo/agent/confirmation_gate.h>

#include <chrono>
#include <utility>

namespace kumo::agent {

bool ConfirmationGate::ask(std::string toolName, std::string argumentsJson,
                           const std::atomic<bool>& abort) {
    std::unique_lock lock(mutex_);
    prompt_ = Prompt{++lastPromptId_, std::move(toolName), std::move(argumentsJson)};
    resolved_ = false;
    approved_ = false;
    // Bounded waits instead of a cancellation latch: the abort flag belongs to
    // the asking session, so its shutdown cannot poison the shared gate.
    while (!resolved_) {
        if (abort.load()) {
            prompt_.reset();
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
    prompt_.reset();
    return approved_;
}

std::optional<ConfirmationGate::Prompt> ConfirmationGate::pending() const {
    std::lock_guard lock(mutex_);
    if (resolved_) {
        return std::nullopt;
    }
    return prompt_;
}

void ConfirmationGate::resolve(std::uint64_t promptId, bool approved) {
    {
        std::lock_guard lock(mutex_);
        if (!prompt_.has_value() || resolved_ || prompt_->id != promptId) {
            return;
        }
        resolved_ = true;
        approved_ = approved;
    }
    cv_.notify_all();
}

} // namespace kumo::agent
