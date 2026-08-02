#include <doctest/doctest.h>

#include <kumo/agent/fake_provider.h>
#include <kumo/agent/session.h>
#include <kumo/agent/summary.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/core/main_thread_queue.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace kumo;
using namespace kumo::agent;

namespace {

constexpr std::chrono::seconds kWait{5};

using Entry = AgentSession::TranscriptEntry;
using Kind = Entry::Kind;

ChatMessage userText(std::string text) {
    ChatMessage message;
    message.role = Role::User;
    message.text = std::move(text);
    return message;
}

ChatMessage assistantText(std::string text) {
    ChatMessage message;
    message.role = Role::Assistant;
    message.text = std::move(text);
    message.stopReason = StopReason::EndTurn;
    return message;
}

ChatMessage assistantToolUse(std::string id, std::string name, std::string args) {
    ChatMessage message;
    message.role = Role::Assistant;
    message.stopReason = StopReason::ToolUse;
    message.toolCalls.push_back({std::move(id), std::move(name), std::move(args)});
    return message;
}

ChatMessage toolResult(std::string callId, std::string contentJson) {
    ChatMessage message;
    message.role = Role::Tool;
    message.toolResults.push_back({std::move(callId), std::move(contentJson), false, {}, {}});
    return message;
}

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

ToolRegistry echoRegistry() {
    ToolRegistry registry;
    registry.add(
        {.name = "echo", .description = "test", .parametersSchema = R"({"type":"object"})"},
        [](std::string_view) { return std::string(R"({"status":"ok"})"); });
    return registry;
}

AgentSession::Desc desc() {
    return {.model = "fake-model", .systemPrompt = "You are a test.", .maxTokens = 512};
}

// Succeeds on the first complete() call, errors on the second (the compression
// request), succeeds again afterwards.
class FailsOnSecondCallProvider final : public ILLMProvider {
public:
    explicit FailsOnSecondCallProvider(ChatMessage reply) : reply_(std::move(reply)) {}

    CompleteResult complete(const ChatRequest& request) override {
        ++calls;
        requestMessageCounts.push_back(request.messages.size());
        if (calls == 2) {
            return std::unexpected(ProviderError{.kind = ProviderError::Kind::Network,
                                                 .message = "connection refused"});
        }
        return reply_;
    }

    int calls = 0;
    std::vector<std::size_t> requestMessageCounts;

private:
    ChatMessage reply_;
};

} // namespace

TEST_CASE("approxTokens estimates bytes/4, summing tool payloads") {
    ChatMessage plain = userText(std::string(8, 'a'));
    CHECK(approxTokens(plain) == 2);

    ChatMessage empty;
    CHECK(approxTokens(empty) == 0);

    ChatMessage tiny = userText("a");
    CHECK(approxTokens(tiny) == 1); // non-empty message floors to at least 1 token.

    ChatMessage withCall = assistantToolUse("1", "echo", std::string(12, 'b'));
    CHECK(approxTokens(withCall) == 3);

    ChatMessage withResult = toolResult("1", std::string(4, 'c'));
    CHECK(approxTokens(withResult) == 1);

    std::vector<ChatMessage> messages{plain, withCall, withResult};
    CHECK(approxTokens(std::span<const ChatMessage>(messages)) == 2 + 3 + 1);
}

TEST_CASE("approxTokens adds a flat per-image cost, not the base64 length") {
    ChatMessage withImage = toolResult("1", R"({"status":"ok"})");
    // A generous base64 stand-in: if the cost tracked its length, this alone
    // would blow past any reasonable compression threshold.
    withImage.toolResults[0].images.push_back(std::string(10000, 'A'));

    const std::size_t textOnly = approxTokens(toolResult("1", R"({"status":"ok"})"));
    // 512 mirrors summary.cpp's kImageTokenEstimate, accumulated per image.
    CHECK(approxTokens(withImage) == textOnly + 512);
    withImage.toolResults[0].images.push_back(std::string(10000, 'B'));
    CHECK(approxTokens(withImage) == textOnly + 1024);
}

TEST_CASE("shouldCompress compares the approx token count against the threshold") {
    std::vector<ChatMessage> messages{userText(std::string(40, 'a'))}; // 40/4 = 10 tokens.
    CHECK(shouldCompress(messages, 10));
    CHECK(shouldCompress(messages, 9));
    CHECK(!shouldCompress(messages, 11));
}

TEST_CASE("compressionCutoff never splits an assistant tool call from its tool result") {
    std::vector<ChatMessage> messages{userText("hi"),         assistantToolUse("c1", "echo", "{}"),
                                      toolResult("c1", "ok"), assistantText("done"),
                                      userText("again"),      assistantText("done again")};

    const std::size_t cutoff = compressionCutoff(messages, 2);
    REQUIRE(cutoff != 0);
    CHECK(messages[cutoff].role != Role::Tool);
    if (cutoff > 0) {
        const ChatMessage& previous = messages[cutoff - 1];
        CHECK(!(previous.role == Role::Assistant && !previous.toolCalls.empty()));
    }
}

TEST_CASE("compressionCutoff never lands on a Role::Tool message") {
    // A naive size-keepRecentMessages would land exactly on index 2 (the tool
    // reply); the algorithm must back off to index 1 instead.
    std::vector<ChatMessage> messages{userText("hi"), assistantToolUse("c1", "echo", "{}"),
                                      toolResult("c1", "ok"), assistantText("done")};

    const std::size_t cutoff = compressionCutoff(messages, 2);
    // Backing off from index 2 lands on index 1, which is < 2 and therefore not
    // worth compressing.
    CHECK(cutoff == 0);
}

TEST_CASE("compressionCutoff avoids splitting a tool pair when there is enough history") {
    std::vector<ChatMessage> messages{userText("a"),          assistantText("b"),
                                      userText("c"),          assistantToolUse("c1", "echo", "{}"),
                                      toolResult("c1", "ok"), assistantText("done")};

    const std::size_t cutoff = compressionCutoff(messages, 2);
    CHECK(cutoff == 3);
    CHECK(messages[cutoff].role != Role::Tool);
}

TEST_CASE("compressionCutoff returns 0 for tiny histories") {
    std::vector<ChatMessage> empty;
    CHECK(compressionCutoff(empty, 0) == 0);
    std::vector<ChatMessage> single{userText("hi")};
    CHECK(compressionCutoff(single, 0) == 0);
    std::vector<ChatMessage> three{userText("a"), assistantText("b"), userText("c")};
    CHECK(compressionCutoff(three, 2) == 0);
}

TEST_CASE("compressionCutoff returns 0 when keepRecentMessages covers the whole history") {
    std::vector<ChatMessage> messages{userText("a"), assistantText("b"), userText("c"),
                                      assistantText("d"), userText("e")};
    CHECK(compressionCutoff(messages, 5) == 0);
    CHECK(compressionCutoff(messages, 10) == 0);
}

TEST_CASE("makeSummaryRequest builds one user message with an English instruction and no tools") {
    std::vector<ChatMessage> prefix{
        userText("please add a red cube"),
        assistantToolUse("c1", "scene_add_entity", R"({"name":"cube"})"),
        toolResult("c1", R"({"entity_id":42})")};

    const ChatRequest request = makeSummaryRequest(prefix, "test-model", 256);
    CHECK(request.model == "test-model");
    CHECK(request.maxTokens == 256);
    CHECK(request.tools.empty());
    CHECK(request.systemPrompt.find("compress") != std::string::npos);
    REQUIRE(request.messages.size() == 1);
    CHECK(request.messages[0].role == Role::User);

    const std::string& text = request.messages[0].text;
    CHECK(text.find("please add a red cube") != std::string::npos);
    CHECK(text.find("scene_add_entity") != std::string::npos);
    CHECK(text.find(R"({"entity_id":42})") != std::string::npos);
    CHECK(text.find("Summarize") != std::string::npos);
}

TEST_CASE("makeSummaryMessage wraps the summary text as a user message") {
    const ChatMessage message = makeSummaryMessage("entity 42 is a red cube at the origin");
    CHECK(message.role == Role::User);
    CHECK(message.text.find("entity 42 is a red cube at the origin") != std::string::npos);
    CHECK(message.text.find("Summary of the earlier conversation") != std::string::npos);
    CHECK(message.stopReason == StopReason::Other);
}

TEST_CASE("AgentSession compresses old history once the token threshold is crossed") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    // The huge first reply alone crosses the threshold, but keepRecentMessages=2
    // keeps the whole 2-message history after just one turn, so nothing fires
    // yet. Later replies stay small so the summary (once produced) drops well
    // below the threshold again and compression does not keep re-triggering.
    const std::string longReply(4000, 'x');
    FakeProvider provider({assistantText(longReply), assistantText("ok"),
                           assistantText("COMPACT SUMMARY"), assistantText("final reply")});
    AgentSession::Desc d = desc();
    d.summaryThresholdTokens = 100;
    d.keepRecentMessages = 2;
    AgentSession session(provider, registry, queue, nullptr, d);

    REQUIRE(session.submit("first"));
    REQUIRE(pumpUntilIdle(queue, session));
    // History is only [user, assistant] (2 messages); keepRecentMessages=2 keeps
    // all of it, so there is nothing worth compressing yet.
    CHECK(provider.requests().size() == 1);

    REQUIRE(session.submit("second"));
    REQUIRE(pumpUntilIdle(queue, session));
    // History is now 4 messages; compression fires after the turn, summarizing
    // the first exchange and keeping the 2 most recent messages.
    REQUIRE(provider.requests().size() == 3);
    const FakeProvider::Recorded& summaryRequest = provider.requests()[2];
    CHECK(summaryRequest.tools.empty());
    REQUIRE(summaryRequest.messages.size() == 1);
    CHECK(summaryRequest.messages[0].text.find("Summarize") != std::string::npos);

    const std::vector<Entry> transcript = session.drainTranscript();
    bool sawInfo = false;
    for (const Entry& entry : transcript) {
        sawInfo = sawInfo ||
                  (entry.kind == Kind::Info && entry.text.find("compressed") != std::string::npos);
    }
    CHECK(sawInfo);

    REQUIRE(session.submit("third"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(provider.requests().size() == 4);
    const std::vector<ChatMessage>& messages = provider.requests().back().messages;
    REQUIRE(!messages.empty());
    CHECK(messages.front().role == Role::User);
    CHECK(messages.front().text.find("COMPACT SUMMARY") != std::string::npos);
}

TEST_CASE("AgentSession keeps full history when the compression request fails") {
    MainThreadQueue queue;
    ToolRegistry registry = echoRegistry();
    FailsOnSecondCallProvider provider(assistantText("normal reply"));
    AgentSession::Desc d = desc();
    d.summaryThresholdTokens = 1;
    d.keepRecentMessages = 0;
    AgentSession session(provider, registry, queue, nullptr, d);

    REQUIRE(session.submit("go"));
    REQUIRE(pumpUntilIdle(queue, session));

    REQUIRE(provider.calls == 2);
    const std::vector<Entry> transcript = session.drainTranscript();
    bool sawInfo = false;
    for (const Entry& entry : transcript) {
        sawInfo = sawInfo || (entry.kind == Kind::Info &&
                              entry.text.find("keeping full history") != std::string::npos);
    }
    CHECK(sawInfo);

    // History must be unchanged: the next turn's request carries the full,
    // uncompressed history plus the new user message. (keepRecentMessages=0
    // means this turn's own compressIfNeeded may fire a 4th call afterwards;
    // that is not what is under test here, so only the 3rd call is checked.)
    REQUIRE(session.submit("again"));
    REQUIRE(pumpUntilIdle(queue, session));
    REQUIRE(provider.requestMessageCounts.size() >= 3);
    CHECK(provider.requestMessageCounts[2] == 3);
}

TEST_CASE("collectReferenceImages keeps the newest images and detail across compression") {
    ChatMessage older;
    older.role = Role::User;
    older.userImages = {{.base64 = "OLD1"}, {.base64 = "OLD2"}};
    older.userImageDetail = "low";
    ChatMessage assistant;
    assistant.role = Role::Assistant;
    assistant.text = "ok";
    ChatMessage newer;
    newer.role = Role::User;
    newer.userImages = {{.base64 = "NEW1"}, {.base64 = "NEW2"}};
    newer.userImageDetail = "high";
    const std::vector<ChatMessage> messages{older, assistant, newer};

    const auto [images, detail] = collectReferenceImages(std::span<const ChatMessage>(messages), 3);
    REQUIRE(images.size() == 3);
    // Newest message wins whole; the older one fills the remaining slot.
    CHECK(images[0].base64 == "OLD2");
    CHECK(images[1].base64 == "NEW1");
    CHECK(images[2].base64 == "NEW2");
    CHECK(detail == "high");

    const auto [none, noDetail] =
        collectReferenceImages(std::span<const ChatMessage>(messages.data(), 2), 0);
    CHECK(none.empty());
    CHECK(images[0].base64 == "OLD2");
    (void)noDetail;
}
