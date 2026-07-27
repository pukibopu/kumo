#include <doctest/doctest.h>

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/fake_provider.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/core/main_thread_queue.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

// Every wait is bounded so a stuck worker fails the test instead of hanging it.
constexpr std::chrono::seconds kWait{5};

using Entry = AgentSession::TranscriptEntry;
using Kind = Entry::Kind;

ChatMessage assistantText(std::string text) {
    ChatMessage message;
    message.role = Role::Assistant;
    message.text = std::move(text);
    message.stopReason = StopReason::EndTurn;
    return message;
}

ChatMessage toolUse(std::string id, std::string name, std::string args) {
    ChatMessage message;
    message.role = Role::Assistant;
    message.stopReason = StopReason::ToolUse;
    message.toolCalls.push_back({std::move(id), std::move(name), std::move(args)});
    return message;
}

// Plays the main thread: drains tool work until the worker goes idle.
bool pumpUntilIdle(MainThreadQueue& queue, AgentSession& session) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
        queue.drain();
        if (!session.busy()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template <typename Fn> bool waitFor(Fn&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

ToolRegistry echoRegistry(std::vector<std::string>* seenArgs = nullptr) {
    ToolRegistry registry;
    registry.add(
        {.name = "echo", .description = "test", .parametersSchema = R"({"type":"object"})"},
        [seenArgs](std::string_view args) {
            if (seenArgs != nullptr) {
                seenArgs->emplace_back(args);
            }
            return std::string(R"({"status":"ok"})");
        });
    return registry;
}

AgentSession::Desc desc(int maxToolRounds = 16) {
    return {.model = "fake-model",
            .systemPrompt = "You are a test.",
            .maxTokens = 512,
            .maxToolRounds = maxToolRounds};
}

} // namespace

TEST_CASE("AgentSession runs a plain text turn and records the request") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FakeProvider provider({assistantText("hello")});
    AgentSession session(provider, registry, queue, nullptr, desc());

    CHECK(!session.busy());
    REQUIRE(session.submit("hi"));
    REQUIRE(pumpUntilIdle(queue, session));

    const std::vector<Entry> transcript = session.drainTranscript();
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].kind == Kind::User);
    CHECK(transcript[0].text == "hi");
    CHECK(transcript[1].kind == Kind::Assistant);
    CHECK(transcript[1].text == "hello");

    REQUIRE(provider.requests().size() == 1);
    const FakeProvider::Recorded& request = provider.requests()[0];
    CHECK(request.model == "fake-model");
    CHECK(request.systemPrompt == "You are a test.");
    CHECK(request.maxTokens == 512);
    REQUIRE(request.tools.size() == 1);
    CHECK(request.tools[0].name == "echo");
    REQUIRE(request.messages.size() == 1);
    CHECK(request.messages[0].role == Role::User);
}

TEST_CASE("AgentSession executes tool calls on the draining thread with matching ids") {
    MainThreadQueue queue;
    std::vector<std::string> seenArgs;
    ToolRegistry registry = echoRegistry(&seenArgs);
    FakeProvider provider({toolUse("c1", "echo", R"({"v":1})"), assistantText("done")});
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    REQUIRE(seenArgs.size() == 1);
    CHECK(seenArgs[0] == R"({"v":1})");

    const std::vector<Entry> transcript = session.drainTranscript();
    REQUIRE(transcript.size() == 4);
    CHECK(transcript[1].kind == Kind::ToolCall);
    CHECK(transcript[1].toolName == "echo");
    CHECK(transcript[2].kind == Kind::ToolResult);
    CHECK(transcript[2].json == R"({"status":"ok"})");
    CHECK(transcript[3].kind == Kind::Assistant);

    // The second request must carry the full round trip with the call id echoed.
    REQUIRE(provider.requests().size() == 2);
    const std::vector<ChatMessage>& messages = provider.requests()[1].messages;
    REQUIRE(messages.size() == 3);
    CHECK(messages[1].role == Role::Assistant);
    REQUIRE(messages[2].role == Role::Tool);
    REQUIRE(messages[2].toolResults.size() == 1);
    CHECK(messages[2].toolResults[0].callId == "c1");
    CHECK(messages[2].toolResults[0].contentJson == R"({"status":"ok"})");
}

TEST_CASE("AgentSession feeds unknown-tool errors back instead of failing the turn") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FakeProvider provider({toolUse("c1", "made_up_tool", "{}"), assistantText("recovered")});
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    REQUIRE(provider.requests().size() == 2);
    const std::vector<ChatMessage>& messages = provider.requests()[1].messages;
    REQUIRE(messages[2].toolResults.size() == 1);
    CHECK(messages[2].toolResults[0].contentJson.find("unknown tool") != std::string::npos);
}

TEST_CASE("AgentSession stops at the tool round limit") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FakeProvider provider(
        {toolUse("c1", "echo", "{}"), toolUse("c2", "echo", "{}"), toolUse("c3", "echo", "{}")});
    AgentSession session(provider, registry, queue, nullptr, desc(2));

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    CHECK(provider.requests().size() == 2);
    const std::vector<Entry> transcript = session.drainTranscript();
    REQUIRE(!transcript.empty());
    CHECK(transcript.back().kind == Kind::Error);
    CHECK(transcript.back().text.find("round limit") != std::string::npos);
}

TEST_CASE("AgentSession surfaces MaxTokens truncation instead of swallowing it") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    ChatMessage truncated = toolUse("c1", "echo", "{}");
    truncated.stopReason = StopReason::MaxTokens;
    FakeProvider provider({std::move(truncated)});
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    // The truncated tool call must not execute.
    CHECK(provider.requests().size() == 1);
    const std::vector<Entry> transcript = session.drainTranscript();
    REQUIRE(!transcript.empty());
    CHECK(transcript.back().kind == Kind::Error);
    CHECK(transcript.back().text.find("max_tokens") != std::string::npos);
}

TEST_CASE("ConfirmationGate denial turns a destructive call into cancelled_by_user") {
    MainThreadQueue queue;
    bool executed = false;
    ToolRegistry registry;
    registry.add({.name = "nuke",
                  .description = "test",
                  .parametersSchema = R"({"type":"object"})",
                  .destructive = true},
                 [&](std::string_view) {
                     executed = true;
                     return std::string(R"({"status":"ok"})");
                 });
    FakeProvider provider({toolUse("c1", "nuke", "{}"), assistantText("ok"),
                           toolUse("c2", "nuke", "{}"), assistantText("ok again")});
    ConfirmationGate gate;
    AgentSession session(provider, registry, queue, &gate, desc());

    REQUIRE(session.submit("first"));
    REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    CHECK(gate.pending()->toolName == "nuke");
    CHECK(session.status() == AgentSession::Status::WaitingForConfirmation);
    // A second submit while the turn is blocked must be rejected, not queued.
    CHECK(!session.submit("second"));

    gate.resolve(false);
    REQUIRE(pumpUntilIdle(queue, session));
    CHECK(!executed);
    REQUIRE(provider.requests().size() == 2);
    CHECK(provider.requests()[1].messages[2].toolResults[0].contentJson ==
          R"({"status":"cancelled_by_user"})");

    // Approval lets the same tool through.
    REQUIRE(session.submit("again"));
    REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    gate.resolve(true);
    REQUIRE(pumpUntilIdle(queue, session));
    CHECK(executed);
}

TEST_CASE("AgentSession destruction unblocks a worker waiting on tool work") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FakeProvider provider({toolUse("c1", "echo", "{}"), assistantText("late")});
    {
        AgentSession session(provider, registry, queue, nullptr, desc());
        REQUIRE(session.submit("go"));
        // The worker posts the tool call and blocks; nobody ever drains.
        REQUIRE(waitFor([&] { return queue.pending() > 0; }));
    }
    // Reaching this point without a hang is the assertion.
    CHECK(queue.pending() == 0);
}

TEST_CASE("AgentSession destruction unblocks a worker waiting for confirmation") {
    MainThreadQueue queue;
    ToolRegistry registry;
    registry.add({.name = "nuke",
                  .description = "test",
                  .parametersSchema = R"({"type":"object"})",
                  .destructive = true},
                 [](std::string_view) { return std::string(R"({"status":"ok"})"); });
    FakeProvider provider({toolUse("c1", "nuke", "{}")});
    ConfirmationGate gate;
    {
        AgentSession session(provider, registry, queue, &gate, desc());
        REQUIRE(session.submit("go"));
        REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    }
    CHECK(!gate.pending().has_value());
}

TEST_CASE("FakeProvider falls back to the exhausted message") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FakeProvider provider({assistantText("scripted")}, "script over");
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("one"));
    REQUIRE(pumpUntilIdle(queue, session));
    CHECK(provider.exhausted());
    REQUIRE(session.submit("two"));
    REQUIRE(pumpUntilIdle(queue, session));

    const std::vector<Entry> transcript = session.drainTranscript();
    REQUIRE(transcript.size() == 4);
    CHECK(transcript[3].text == "script over");
}
