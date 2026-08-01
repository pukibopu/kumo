#include <doctest/doctest.h>

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/fake_provider.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/core/main_thread_queue.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
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

class FailingProvider final : public ILLMProvider {
public:
    CompleteResult complete(const ChatRequest& request) override {
        messageCounts.push_back(request.messages.size());
        return std::unexpected(
            ProviderError{.kind = ProviderError::Kind::Network, .message = "connection refused"});
    }

    std::vector<std::size_t> messageCounts;
};

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

TEST_CASE("AgentSession stops at the tool round limit without executing the final calls") {
    MainThreadQueue queue;
    std::vector<std::string> seenArgs;
    ToolRegistry registry = echoRegistry(&seenArgs);
    FakeProvider provider(
        {toolUse("c1", "echo", "{}"), toolUse("c2", "echo", "{}"), toolUse("c3", "echo", "{}")});
    AgentSession session(provider, registry, queue, nullptr, desc(2));

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    CHECK(provider.requests().size() == 2);
    // The final round's calls are stripped, not run: results could never be
    // sent back, and the scene must not mutate behind the model.
    CHECK(seenArgs.size() == 1);
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

    // The truncated calls must not poison later requests as dangling tool_use.
    REQUIRE(session.submit("again"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(provider.requests().size() == 2);
    for (const ChatMessage& message : provider.requests()[1].messages) {
        CHECK(message.toolCalls.empty());
    }
}

TEST_CASE("AgentSession rolls back the user message when the provider errors") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FailingProvider provider;
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("one"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(session.submit("two"));
    REQUIRE(pumpUntilIdle(queue, session));

    // Each attempt must carry exactly one user message: the failed turn's
    // message is rolled back, never left to stack up as consecutive users.
    REQUIRE(provider.messageCounts.size() == 2);
    CHECK(provider.messageCounts[0] == 1);
    CHECK(provider.messageCounts[1] == 1);

    const std::vector<Entry> transcript = session.drainTranscript();
    bool sawError = false;
    for (const Entry& entry : transcript) {
        sawError = sawError || (entry.kind == Kind::Error &&
                                entry.text.find("connection refused") != std::string::npos);
    }
    CHECK(sawError);
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

    gate.resolve(gate.pending()->id, false);
    REQUIRE(pumpUntilIdle(queue, session));
    CHECK(!executed);
    REQUIRE(provider.requests().size() == 2);
    CHECK(provider.requests()[1].messages[2].toolResults[0].contentJson ==
          R"({"status":"cancelled_by_user"})");

    // Approval lets the same tool through.
    REQUIRE(session.submit("again"));
    REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    gate.resolve(gate.pending()->id, true);
    REQUIRE(pumpUntilIdle(queue, session));
    CHECK(executed);
}

TEST_CASE("ConfirmationGate ignores decisions for prompts it is not showing") {
    ConfirmationGate gate;
    std::atomic<bool> abortFlag{false};
    bool approved = true;
    std::thread asker([&] { approved = gate.ask("nuke", "{}", abortFlag); });

    REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    const std::uint64_t id = gate.pending()->id;
    // A stale or forged id (e.g. a double-click racing the next prompt) must
    // not answer the prompt on screen.
    gate.resolve(id + 1, true);
    CHECK(gate.pending().has_value());
    gate.resolve(id, false);
    asker.join();
    CHECK(!approved);
    CHECK(!gate.pending().has_value());
}

TEST_CASE("ConfirmationGate queues concurrent askers and resolves each by its own id") {
    ConfirmationGate gate;
    std::atomic<bool> abortFlag{false};
    bool firstApproved = true;
    bool secondApproved = false;
    std::thread firstAsker([&] { firstApproved = gate.ask("nuke_a", "{}", abortFlag); });

    REQUIRE(waitFor([&] { return gate.pending().has_value(); }));
    const std::uint64_t firstId = gate.pending()->id;
    CHECK(gate.pending()->toolName == "nuke_a");

    std::thread secondAsker([&] { secondApproved = gate.ask("nuke_b", "{}", abortFlag); });
    // The second ask must queue behind the first rather than clobber it: the
    // prompt on screen stays the first one while both askers wait.
    for (int i = 0; i < 20; ++i) {
        REQUIRE(gate.pending().has_value());
        CHECK(gate.pending()->id == firstId);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    gate.resolve(firstId, false);
    firstAsker.join();
    CHECK(!firstApproved);

    // Deciding the first prompt must not touch the second: it only now becomes
    // the one showing, and only its own id resolves it.
    REQUIRE(waitFor(
        [&] { return gate.pending().has_value() && gate.pending()->toolName == "nuke_b"; }));
    const std::uint64_t secondId = gate.pending()->id;
    CHECK(secondId != firstId);

    gate.resolve(secondId, true);
    secondAsker.join();
    CHECK(secondApproved);
    CHECK(!gate.pending().has_value());
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
    // Reaching this point without a hang is the assertion. The undrained item
    // captures only app-owned state, so running it late is safe and the queue
    // remains usable for a future session.
    queue.drain();
    CHECK(queue.pending() == 0);
    std::future<std::string> after = queue.post([] { return std::string("alive"); });
    queue.drain();
    CHECK(after.get() == "alive");
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

TEST_CASE("AgentSession attaches a tool result's image and retains only the newest one") {
    MainThreadQueue queue;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "kumo_agent_session_vision_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path path1 = dir / "shot1.png";
    const std::filesystem::path path2 = dir / "shot2.png";
    std::ofstream(path1, std::ios::binary) << "IMG1DATA";
    std::ofstream(path2, std::ios::binary) << "IMG2DATA";

    ToolRegistry registry;
    int calls = 0;
    registry.add({.name = "take_screenshot",
                  .description = "test",
                  .parametersSchema = R"({"type":"object"})"},
                 [&](std::string_view) {
                     ++calls;
                     const std::string path = (calls == 1 ? path1 : path2).string();
                     return nlohmann::json{{"status", "ok"}, {"image_path", path}}.dump();
                 });

    FakeProvider provider({toolUse("c1", "take_screenshot", "{}"), assistantText("done1"),
                           toolUse("c2", "take_screenshot", "{}"), assistantText("done2")});
    AgentSession session(provider, registry, queue, nullptr, desc());

    REQUIRE(session.submit("first"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(provider.requests().size() == 2);
    {
        const std::vector<ChatMessage>& messages = provider.requests()[1].messages;
        REQUIRE(messages.size() == 3);
        REQUIRE(messages[2].role == Role::Tool);
        REQUIRE(messages[2].toolResults.size() == 1);
        // base64("IMG1DATA"), computed independently of the encoder under test.
        CHECK(messages[2].toolResults[0].imagePngBase64 == "SU1HMURBVEE=");
    }

    REQUIRE(session.submit("second"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(provider.requests().size() == 4);
    {
        // Requested before the second screenshot ran: the first image is still
        // the only one attached anywhere in history.
        const std::vector<ChatMessage>& messages = provider.requests()[2].messages;
        REQUIRE(messages.size() == 5);
        REQUIRE(messages[2].role == Role::Tool);
        CHECK(messages[2].toolResults[0].imagePngBase64 == "SU1HMURBVEE=");
    }
    {
        // After the second screenshot lands, retention evicts the first image so
        // only the newest stays attached.
        const std::vector<ChatMessage>& messages = provider.requests()[3].messages;
        REQUIRE(messages.size() == 7);
        REQUIRE(messages[2].role == Role::Tool);
        CHECK(messages[2].toolResults[0].imagePngBase64.empty());
        REQUIRE(messages[6].role == Role::Tool);
        // base64("IMG2DATA")
        CHECK(messages[6].toolResults[0].imagePngBase64 == "SU1HMkRBVEE=");
    }

    std::filesystem::remove_all(dir);
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
