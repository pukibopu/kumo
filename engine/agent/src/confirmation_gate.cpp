#include <kumo/agent/confirmation_gate.h>

#include <utility>

namespace kumo::agent {

bool ConfirmationGate::ask(Prompt prompt) {
    std::unique_lock lock(mutex_);
    if (cancelled_) {
        return false;
    }
    prompt_ = std::move(prompt);
    resolved_ = false;
    approved_ = false;
    cv_.wait(lock, [this] { return resolved_ || cancelled_; });
    prompt_.reset();
    return approved_ && !cancelled_;
}

std::optional<ConfirmationGate::Prompt> ConfirmationGate::pending() const {
    std::lock_guard lock(mutex_);
    if (resolved_ || cancelled_) {
        return std::nullopt;
    }
    return prompt_;
}

void ConfirmationGate::resolve(bool approved) {
    {
        std::lock_guard lock(mutex_);
        if (!prompt_.has_value() || resolved_) {
            return;
        }
        resolved_ = true;
        approved_ = approved;
    }
    cv_.notify_all();
}

void ConfirmationGate::cancelAll() {
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

} // namespace kumo::agent
