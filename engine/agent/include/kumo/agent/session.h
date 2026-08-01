#pragma once

#include <kumo/agent/provider.h>
#include <kumo/agent/tool_registry.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kumo {
class MainThreadQueue;
}

namespace kumo::agent {

class ConfirmationGate;

// Owns one conversation with one model on a dedicated worker thread. submit()
// hands a user message to the worker, which loops complete() and tool execution
// (marshalled through MainThreadQueue, so the worker never holds engine state)
// until the model stops asking for tools or maxToolRounds is hit (ADR 0005).
// drainTranscript() moves out the UI-facing mirror of that exchange.
//
// This API is the seam a future product shell consumes, so it must stay free of
// UI and platform types (ADR 0043). All referenced objects must outlive the
// session; in a composition root, declare it after the scene, renderer and queue.
// Shutdown never latches state onto the queue or gate: the worker polls abort_
// while waiting, so both stay usable by other sessions.
class AgentSession {
public:
    enum class Status { Idle, WaitingForModel, RunningTool, WaitingForConfirmation };

    struct Desc {
        std::string model;
        std::string systemPrompt;
        std::uint32_t maxTokens = 4096;
        int maxToolRounds = 16;
        // 0 disables history compression.
        std::size_t summaryThresholdTokens = 0;
        int keepRecentMessages = 8;
        // Pushed as a Kind::Info transcript entry as soon as the session is
        // constructed; empty for a no-op. Lets a caller that just rebuilt this
        // session (e.g. a hot config reload) explain the fresh, historyless
        // transcript instead of leaving it silently empty.
        std::string initialNotice;
    };

    struct TranscriptEntry {
        enum class Kind { User, Assistant, ToolCall, ToolResult, Error, Info };
        Kind kind = Kind::Assistant;
        // User/Assistant text or the error message.
        std::string text;
        // ToolCall/ToolResult only.
        std::string toolName;
        // ToolCall arguments or ToolResult content.
        std::string json;
    };

    // `confirm` may be null: destructive tools then run unprompted (ADR 0022).
    AgentSession(ILLMProvider& provider, const ToolRegistry& registry, MainThreadQueue& queue,
                 ConfirmationGate* confirm, Desc desc);
    ~AgentSession();

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    // False when a turn is already running; the message is not queued.
    bool submit(std::string userText);
    bool busy() const;
    Status status() const;
    std::vector<TranscriptEntry> drainTranscript();

private:
    void workerLoop();
    void runTurn(std::string userText);
    void compressIfNeeded();
    ToolResult executeToolCall(const ToolCall& call);
    std::string awaitJson(std::future<std::string> future);
    void pushEntry(TranscriptEntry entry);
    void setStatus(Status status);

    ILLMProvider& provider_;
    const ToolRegistry& registry_;
    MainThreadQueue& queue_;
    ConfirmationGate* confirm_ = nullptr;
    Desc desc_;

    // Worker-owned once the thread starts.
    std::vector<ChatMessage> history_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string pendingUserText_;
    bool hasPending_ = false;
    bool busy_ = false;
    bool stopping_ = false;
    Status status_ = Status::Idle;
    std::vector<TranscriptEntry> transcript_;
    std::atomic<bool> abort_{false};

    // Last member: started after every other member is initialized.
    std::thread worker_;
};

} // namespace kumo::agent
