#include <doctest/doctest.h>

#include <kumo/agent/claude_codec.h>
#include <kumo/core/file.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace kumo;
using namespace kumo::agent;
using nlohmann::json;

namespace {

json readFixture(const char* name) {
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / "claude" / name;
    const auto text = readTextFile(path);
    REQUIRE_MESSAGE(text.has_value(), path.string());
    const json parsed = json::parse(*text, nullptr, false);
    REQUIRE(!parsed.is_discarded());
    return parsed;
}

std::string fixtureText(const char* name) {
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / "claude" / name;
    const auto text = readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

struct TestRequest {
    std::vector<ToolDef> tools;
    std::vector<ChatMessage> messages;
    ChatRequest request;

    TestRequest() {
        tools.push_back({.name = "scene_add_entity",
                         .description = "Add an entity.",
                         .parametersSchema = R"({"type":"object","properties":)"
                                             R"({"primitive":{"type":"string"}},)"
                                             R"("required":["primitive"]})"});
        ChatMessage user;
        user.role = Role::User;
        user.text = "add a sphere";
        messages.push_back(user);

        ChatMessage assistant;
        assistant.role = Role::Assistant;
        assistant.stopReason = StopReason::ToolUse;
        assistant.toolCalls.push_back({"c1", "scene_add_entity", R"({"primitive":"sphere"})"});
        messages.push_back(assistant);

        ChatMessage toolResult;
        toolResult.role = Role::Tool;
        toolResult.toolResults.push_back({"c1", R"({"status":"ok","entity_id":"0:0"})", false, ""});
        messages.push_back(toolResult);

        ChatMessage followUp;
        followUp.role = Role::User;
        followUp.text = "make it red";
        messages.push_back(followUp);

        request.model = "test-model";
        request.systemPrompt = "You are a test.";
        request.messages = messages;
        request.tools = tools;
        request.maxTokens = 256;
    }
};

} // namespace

TEST_CASE("claude codec encodes the canonical request with same-role merging") {
    const TestRequest fixture;
    // The trailing user text must merge into the tool_result user message: the
    // API rejects consecutive same-role messages.
    const json encoded = json::parse(encodeMessagesRequest(fixture.request));
    CHECK(encoded == readFixture("request_tools.json"));
}

TEST_CASE("claude codec turns an image-bearing tool result into a text+image content array") {
    std::vector<ToolDef> tools{{.name = "viewer_screenshot",
                                .description = "Take a screenshot.",
                                .parametersSchema = R"({"type":"object","properties":{}})"}};

    ChatMessage assistant;
    assistant.role = Role::Assistant;
    assistant.stopReason = StopReason::ToolUse;
    assistant.toolCalls.push_back({"c1", "viewer_screenshot", "{}"});

    ChatMessage toolResult;
    toolResult.role = Role::Tool;
    toolResult.toolResults.push_back({.callId = "c1",
                                      .contentJson = R"({"status":"ok","width":640,"height":360})",
                                      .isError = false,
                                      // base64("PNGDATA"), independent of the encoder under test.
                                      .imagePngBase64 = "UE5HREFUQQ=="});

    ChatRequest request;
    request.model = "test-model";
    request.systemPrompt = "You are a test.";
    request.messages = {assistant, toolResult};
    request.tools = tools;
    request.maxTokens = 256;

    const json encoded = json::parse(encodeMessagesRequest(request));
    CHECK(encoded == readFixture("request_image.json"));
}

TEST_CASE("claude codec decodes a plain text reply") {
    const auto message = decodeMessagesResponse(fixtureText("response_text.json"));
    REQUIRE(message.has_value());
    CHECK(message->role == Role::Assistant);
    CHECK(message->text == "场景已经准备好了。");
    CHECK(message->toolCalls.empty());
    CHECK(message->stopReason == StopReason::EndTurn);
}

TEST_CASE("claude codec decodes a tool_use block") {
    const auto message = decodeMessagesResponse(fixtureText("response_tool_use.json"));
    REQUIRE(message.has_value());
    CHECK(message->text.empty());
    REQUIRE(message->toolCalls.size() == 1);
    CHECK(message->toolCalls[0].id == "toolu_abc123");
    CHECK(message->toolCalls[0].name == "scene_add_entity");
    const json arguments = json::parse(message->toolCalls[0].argumentsJson);
    CHECK(arguments["primitive"] == "sphere");
    CHECK(arguments["size"] == doctest::Approx(1.0));
    CHECK(message->stopReason == StopReason::ToolUse);
}

TEST_CASE("claude codec decodes mixed text and tool_use content") {
    const auto message = decodeMessagesResponse(fixtureText("response_mixed.json"));
    REQUIRE(message.has_value());
    CHECK(message->text == "我先看一下场景。");
    REQUIRE(message->toolCalls.size() == 1);
    CHECK(message->toolCalls[0].name == "scene_list");
    CHECK(message->stopReason == StopReason::ToolUse);
}

TEST_CASE("claude codec maps stop_reason max_tokens to MaxTokens") {
    const auto message = decodeMessagesResponse(fixtureText("response_max_tokens.json"));
    REQUIRE(message.has_value());
    CHECK(message->stopReason == StopReason::MaxTokens);
}

TEST_CASE("claude codec rejects garbage without throwing") {
    CHECK(!decodeMessagesResponse("not json").has_value());
    CHECK(!decodeMessagesResponse("[]").has_value());
    CHECK(!decodeMessagesResponse("{}").has_value());
    // Null stop_reason must decode as Other, not throw.
    const auto message =
        decodeMessagesResponse(R"({"content":[{"type":"text","text":"hi"}],"stop_reason":null})");
    REQUIRE(message.has_value());
    CHECK(message->stopReason == StopReason::Other);
}
