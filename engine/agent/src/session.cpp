#include <kumo/agent/session.h>

#include "base64.h"

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/summary.h>
#include <kumo/core/main_thread_queue.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace kumo::agent {

namespace {
// Mirrors the app-side attachment cap; bounds what compression carries forward.
constexpr std::size_t kMaxCarriedReferenceImages = 3;
} // namespace

AgentSession::AgentSession(ILLMProvider& provider, const ToolRegistry& registry,
                           MainThreadQueue& queue, ConfirmationGate* confirm, Desc desc)
    : provider_(provider), registry_(registry), queue_(queue), confirm_(confirm),
      desc_(std::move(desc)), worker_([this] { workerLoop(); }) {
    if (!desc_.initialNotice.empty()) {
        pushEntry({.kind = TranscriptEntry::Kind::Info, .text = desc_.initialNotice});
    }
}

AgentSession::~AgentSession() {
    // The worker unblocks itself: awaitJson() and ConfirmationGate::ask() poll
    // abort_, so shutdown never latches cancel state onto the app-owned queue or
    // gate (another session may share them).
    abort_.store(true);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    // Unblocks a worker sitting in a long provider round trip (HTTP retries).
    provider_.abort();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AgentSession::submit(std::string userText) {
    return submit(std::move(userText), {}, {});
}

bool AgentSession::submit(std::string userText, std::vector<UserImage> images,
                          std::string imageDetail) {
    {
        std::lock_guard lock(mutex_);
        if (busy_ || stopping_) {
            return false;
        }
        busy_ = true;
        status_ = Status::WaitingForModel;
        pendingUserText_ = std::move(userText);
        pendingImages_ = std::move(images);
        pendingImageDetail_ = std::move(imageDetail);
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

std::size_t AgentSession::completedTurns() const {
    std::lock_guard lock(mutex_);
    return completedTurns_;
}

std::string AgentSession::lastAssistantText() const {
    std::lock_guard lock(mutex_);
    return lastAssistantText_;
}

void AgentSession::workerLoop() {
    for (;;) {
        std::string userText;
        std::vector<UserImage> images;
        std::string imageDetail;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || hasPending_; });
            if (stopping_) {
                return;
            }
            userText = std::move(pendingUserText_);
            images = std::move(pendingImages_);
            imageDetail = std::move(pendingImageDetail_);
            hasPending_ = false;
        }
        {
            std::lock_guard lock(mutex_);
            lastAssistantText_.clear();
        }
        runTurn(std::move(userText), std::move(images), std::move(imageDetail));
        compressIfNeeded();
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
            status_ = Status::Idle;
            ++completedTurns_;
        }
    }
}

void AgentSession::runTurn(std::string userText, std::vector<UserImage> images,
                           std::string imageDetail) {
    const std::size_t turnStart = history_.size();
    pushEntry({.kind = TranscriptEntry::Kind::User, .text = userText});
    ChatMessage userMessage;
    userMessage.role = Role::User;
    userMessage.text = std::move(userText);
    userMessage.userImages = std::move(images);
    userMessage.userImageDetail = std::move(imageDetail);
    history_.push_back(std::move(userMessage));

    for (int round = 0; round < desc_.maxToolRounds; ++round) {
        if (abort_.load()) {
            return;
        }
        setStatus(Status::WaitingForModel);
        ChatRequest request;
        request.model = desc_.model;
        request.systemPrompt = desc_.systemPrompt;
        request.messages = history_;
        request.tools = registry_.defs();
        request.maxTokens = desc_.maxTokens;
        request.reasoningEffort = desc_.reasoningEffort;
        CompleteResult result = provider_.complete(request);
        if (abort_.load()) {
            return;
        }
        if (!result.has_value()) {
            pushEntry({.kind = TranscriptEntry::Kind::Error, .text = result.error().message});
            // Roll back the dangling user message so a retry does not put two
            // consecutive user messages on the wire. Later-round state is kept:
            // its tools already ran, and history ending on tool results is
            // something the codecs handle.
            if (history_.size() == turnStart + 1) {
                history_.resize(turnStart);
            }
            return;
        }

        const bool hadCalls = !result->toolCalls.empty();
        const bool wantsTools = result->stopReason == StopReason::ToolUse && hadCalls;
        // A truncated or otherwise-ended reply may carry incomplete calls, and
        // the final round must not execute (its results could never be sent
        // back). Either way the calls are stripped: an assistant tool_use with
        // no matching tool_result poisons every later request.
        const bool execute = wantsTools && round + 1 < desc_.maxToolRounds;
        if (!execute) {
            result->toolCalls.clear();
        }
        if (!result->text.empty()) {
            pushEntry({.kind = TranscriptEntry::Kind::Assistant, .text = result->text});
        }
        for (const ToolCall& call : result->toolCalls) {
            pushEntry({.kind = TranscriptEntry::Kind::ToolCall,
                       .toolName = call.name,
                       .json = call.argumentsJson});
        }
        const StopReason stop = result->stopReason;
        if (!result->text.empty() || !result->toolCalls.empty()) {
            history_.push_back(std::move(*result));
        }
        if (!execute) {
            if (stop == StopReason::MaxTokens) {
                pushEntry({.kind = TranscriptEntry::Kind::Error,
                           .text = "response truncated by max_tokens"});
            } else if (wantsTools) {
                pushEntry({.kind = TranscriptEntry::Kind::Error,
                           .text = std::format("tool round limit ({}) reached; calls not executed",
                                               desc_.maxToolRounds)});
            } else if (hadCalls) {
                pushEntry({.kind = TranscriptEntry::Kind::Error,
                           .text = "tool calls dropped: reply did not stop for tool use"});
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
        // Retention: images are resent with the whole history every round, so at
        // most one stays attached at a time. A fresh image evicts every earlier
        // one rather than the other way around, since the newest screenshot is
        // the one worth the model's attention.
        const bool carriesImage =
            std::any_of(toolMessage.toolResults.begin(), toolMessage.toolResults.end(),
                        [](const ToolResult& toolResult) { return !toolResult.images.empty(); });
        if (carriesImage) {
            for (ChatMessage& earlier : history_) {
                for (ToolResult& earlierResult : earlier.toolResults) {
                    earlierResult.images.clear();
                }
            }
        }
        history_.push_back(std::move(toolMessage));
    }
}

void AgentSession::compressIfNeeded() {
    if (desc_.summaryThresholdTokens == 0 || abort_.load()) {
        return;
    }
    if (!shouldCompress(history_, desc_.summaryThresholdTokens)) {
        return;
    }
    const auto keep = static_cast<std::size_t>(std::max(desc_.keepRecentMessages, 0));
    const std::size_t cutoff = compressionCutoff(history_, keep);
    if (cutoff == 0) {
        return;
    }

    setStatus(Status::WaitingForModel);
    const ChatRequest request = makeSummaryRequest(
        std::span<const ChatMessage>(history_.data(), cutoff), desc_.model, desc_.maxTokens);
    const CompleteResult result = provider_.complete(request);
    if (abort_.load()) {
        return;
    }
    if (result.has_value() && !result->text.empty()) {
        // Reference images in the compressed range would vanish with their
        // messages; re-attach the newest ones to the summary (MB review).
        auto [referenceImages, referenceDetail] = collectReferenceImages(
            std::span<const ChatMessage>(history_.data(), cutoff), kMaxCarriedReferenceImages);
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(cutoff));
        ChatMessage summary = makeSummaryMessage(result->text);
        summary.userImages = std::move(referenceImages);
        summary.userImageDetail = std::move(referenceDetail);
        history_.insert(history_.begin(), std::move(summary));
        pushEntry({.kind = TranscriptEntry::Kind::Info,
                   .text = std::format("compressed {} earlier messages into a summary", cutoff)});
    } else {
        pushEntry({.kind = TranscriptEntry::Kind::Info,
                   .text = "history compression failed; keeping full history"});
    }
}

ToolResult AgentSession::executeToolCall(const ToolCall& call) {
    ToolResult result;
    result.callId = call.id;
    const ToolDef* def = registry_.find(call.name);
    if (def != nullptr && def->destructive && confirm_ != nullptr) {
        setStatus(Status::WaitingForConfirmation);
        if (!confirm_->ask(call.name, call.argumentsJson, abort_)) {
            result.contentJson = R"({"status":"cancelled_by_user"})";
            return result;
        }
    }
    setStatus(Status::RunningTool);
    // An unknown tool also goes through invoke(): its error JSON is protocol
    // payload the model corrects itself with (ADR 0028). The work captures only
    // app-owned state, so it stays runnable even if this session dies first.
    std::future<std::string> future =
        queue_.post([registry = &registry_, name = call.name, args = call.argumentsJson] {
            return registry->invoke(name, args);
        });
    result.contentJson = awaitJson(std::move(future));

    // Vision feedback (M6.97, multi-image MB-1): image_paths (or legacy
    // image_path) attach as base64; unreadable files degrade to text-only.
    const nlohmann::json parsed = nlohmann::json::parse(result.contentJson, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
        std::vector<std::string> paths;
        const auto pathsIt = parsed.find("image_paths");
        if (pathsIt != parsed.end() && pathsIt->is_array()) {
            for (const nlohmann::json& entry : *pathsIt) {
                if (entry.is_string()) {
                    paths.push_back(entry.get<std::string>());
                }
            }
        } else if (const auto pathIt = parsed.find("image_path");
                   pathIt != parsed.end() && pathIt->is_string()) {
            paths.push_back(pathIt->get<std::string>());
        }
        for (const std::string& path : paths) {
            if (std::optional<std::string> encoded = detail::base64EncodeFile(path);
                encoded.has_value()) {
                result.images.push_back(std::move(*encoded));
            }
        }
        const auto detailIt = parsed.find("image_detail");
        if (detailIt != parsed.end() && detailIt->is_string()) {
            result.imageDetail = detailIt->get<std::string>();
        }
    }
    return result;
}

std::string AgentSession::awaitJson(std::future<std::string> future) {
    // Bounded waits so an aborted session stops waiting on work nobody drains;
    // the queued item is fulfilled by a later drain or the queue's destructor.
    while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
        if (abort_.load()) {
            return errorJson("cancelled: shutting down");
        }
    }
    // MainThreadQueue never leaves a broken promise, so get() cannot throw.
    return future.get();
}

void AgentSession::pushEntry(TranscriptEntry entry) {
    std::lock_guard lock(mutex_);
    if (entry.kind == TranscriptEntry::Kind::Assistant) {
        lastAssistantText_ = entry.text;
    }
    transcript_.push_back(std::move(entry));
}

void AgentSession::setStatus(Status status) {
    std::lock_guard lock(mutex_);
    status_ = status;
}

} // namespace kumo::agent
