#include <kumo/agent/session.h>

#include <kumo/agent/confirmation_gate.h>
#include <kumo/core/main_thread_queue.h>

#include <exception>
#include <format>
#include <utility>

namespace kumo::agent {

AgentSession::AgentSession(ILLMProvider& provider, const ToolRegistry& registry,
                           MainThreadQueue& queue, ConfirmationGate* confirm, Desc desc)
    : provider_(provider), registry_(registry), queue_(queue), confirm_(confirm),
      desc_(std::move(desc)), worker_([this] { workerLoop(); }) {}

AgentSession::~AgentSession() {
    abort_.store(true);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (confirm_ != nullptr) {
        confirm_->cancelAll();
    }
    // Unblocks a worker waiting on a tool future; the queue stays cancelled, which
    // is fine because the session is the only poster and is going away.
    queue_.cancelAll(R"({"status":"error","message":"cancelled: shutting down"})");
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AgentSession::submit(std::string userText) {
    {
        std::lock_guard lock(mutex_);
        if (busy_ || stopping_) {
            return false;
        }
        busy_ = true;
        status_ = Status::WaitingForModel;
        pendingUserText_ = std::move(userText);
        hasPending_ = true;
    }
    cv_.notify_all();
    return true;
}

bool AgentSession::busy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

AgentSession::Status AgentSession::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

std::vector<AgentSession::TranscriptEntry> AgentSession::drainTranscript() {
    std::lock_guard lock(mutex_);
    return std::exchange(transcript_, {});
}

void AgentSession::workerLoop() {
    for (;;) {
        std::string userText;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || hasPending_; });
            if (stopping_) {
                return;
            }
            userText = std::move(pendingUserText_);
            hasPending_ = false;
        }
        runTurn(std::move(userText));
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
            status_ = Status::Idle;
        }
    }
}

void AgentSession::runTurn(std::string userText) {
    pushEntry({.kind = TranscriptEntry::Kind::User, .text = userText});
    ChatMessage userMessage;
    userMessage.role = Role::User;
    userMessage.text = std::move(userText);
    history_.push_back(std::move(userMessage));

    for (int round = 0; round < desc_.maxToolRounds; ++round) {
        setStatus(Status::WaitingForModel);
        ChatRequest request;
        request.model = desc_.model;
        request.systemPrompt = desc_.systemPrompt;
        request.messages = history_;
        request.tools = registry_.defs();
        request.maxTokens = desc_.maxTokens;
        CompleteResult result = provider_.complete(request);
        if (abort_.load()) {
            return;
        }
        if (!result.has_value()) {
            pushEntry({.kind = TranscriptEntry::Kind::Error, .text = result.error().message});
            return;
        }

        if (!result->text.empty()) {
            pushEntry({.kind = TranscriptEntry::Kind::Assistant, .text = result->text});
        }
        for (const ToolCall& call : result->toolCalls) {
            pushEntry({.kind = TranscriptEntry::Kind::ToolCall,
                       .toolName = call.name,
                       .json = call.argumentsJson});
        }
        // A truncated reply may carry incomplete tool calls, so MaxTokens ends the
        // turn even when calls are present (never silently swallowed).
        const bool wantsTools =
            result->stopReason == StopReason::ToolUse && !result->toolCalls.empty();
        history_.push_back(std::move(*result));
        if (!wantsTools) {
            if (history_.back().stopReason == StopReason::MaxTokens) {
                pushEntry({.kind = TranscriptEntry::Kind::Error,
                           .text = "response truncated by max_tokens"});
            }
            return;
        }

        ChatMessage toolMessage;
        toolMessage.role = Role::Tool;
        for (const ToolCall& call : history_.back().toolCalls) {
            if (abort_.load()) {
                return;
            }
            ToolResult toolResult = executeToolCall(call);
            pushEntry({.kind = TranscriptEntry::Kind::ToolResult,
                       .toolName = call.name,
                       .json = toolResult.contentJson});
            toolMessage.toolResults.push_back(std::move(toolResult));
        }
        history_.push_back(std::move(toolMessage));
    }
    pushEntry({.kind = TranscriptEntry::Kind::Error,
               .text = std::format("tool round limit ({}) reached", desc_.maxToolRounds)});
}

ToolResult AgentSession::executeToolCall(const ToolCall& call) {
    ToolResult result;
    result.callId = call.id;
    const ToolDef* def = registry_.find(call.name);
    if (def != nullptr && def->destructive && confirm_ != nullptr) {
        setStatus(Status::WaitingForConfirmation);
        if (!confirm_->ask({.toolName = call.name, .argumentsJson = call.argumentsJson})) {
            result.contentJson = R"({"status":"cancelled_by_user"})";
            return result;
        }
    }
    setStatus(Status::RunningTool);
    // An unknown tool also goes through invoke(): its error JSON is protocol
    // payload the model corrects itself with (ADR 0028).
    std::future<std::string> future =
        queue_.post([this, name = call.name, args = call.argumentsJson] {
            return registry_.invoke(name, args);
        });
    result.contentJson = awaitJson(std::move(future));
    return result;
}

std::string AgentSession::awaitJson(std::future<std::string> future) {
    try {
        return future.get();
    } catch (const std::exception&) {
        // MainThreadQueue never leaves a broken promise; this is the ADR 0035
        // boundary catch in case that guarantee is ever violated.
        return R"({"status":"error","message":"tool execution unavailable"})";
    }
}

void AgentSession::pushEntry(TranscriptEntry entry) {
    std::lock_guard lock(mutex_);
    transcript_.push_back(std::move(entry));
}

void AgentSession::setStatus(Status status) {
    std::lock_guard lock(mutex_);
    status_ = status;
}

} // namespace kumo::agent
