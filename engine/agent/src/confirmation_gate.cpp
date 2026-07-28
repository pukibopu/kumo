#include <kumo/agent/confirmation_gate.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace kumo::agent {

bool ConfirmationGate::ask(std::string toolName, std::string argumentsJson,
                           const std::atomic<bool>& abort) {
    std::unique_lock lock(mutex_);
    const std::uint64_t id = ++lastPromptId_;
    pending_.push_back(Prompt{id, std::move(toolName), std::move(argumentsJson)});
    // Bounded waits instead of a cancellation latch: the abort flag belongs to
    // the asking session, so its shutdown cannot poison the shared gate.
    while (true) {
        if (const auto it = decisions_.find(id); it != decisions_.end()) {
            const bool approved = it->second;
            decisions_.erase(it);
            return approved;
        }
        if (abort.load()) {
            const auto it = std::find_if(pending_.begin(), pending_.end(),
                                         [id](const Prompt& p) { return p.id == id; });
            if (it != pending_.end()) {
                pending_.erase(it);
            }
            decisions_.erase(id);
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
}

std::optional<ConfirmationGate::Prompt> ConfirmationGate::pending() const {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) {
        return std::nullopt;
    }
    return pending_.front();
}

void ConfirmationGate::resolve(std::uint64_t promptId, bool approved) {
    {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(pending_.begin(), pending_.end(),
                                     [promptId](const Prompt& p) { return p.id == promptId; });
        if (it == pending_.end()) {
            return;
        }
        pending_.erase(it);
        decisions_[promptId] = approved;
    }
    cv_.notify_all();
}

} // namespace kumo::agent
