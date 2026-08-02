#include <doctest/doctest.h>

#include <kumo/agent/openai_codec.h>
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
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / "openai" / name;
    const auto text = readTextFile(path);
    REQUIRE_MESSAGE(text.has_value(), path.string());
    const json parsed = json::parse(*text, nullptr, false);
    REQUIRE(!parsed.is_discarded());
    return parsed;
}

std::string fixtureText(const char* name) {
    const auto path = std::filesystem::path(KUMO_FIXTURE_DIR) / "agent" / "openai" / name;
    const auto text = readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

// The canonical request every codec pins: system prompt, a full tool round trip
// and a follow-up user message.
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
        toolResult.toolResults.push_back(
            {"c1", R"({"status":"ok","entity_id":"0:0"})", false, {}, {}});
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

TEST_CASE("openai codec encodes the canonical request") {
    const TestRequest fixture;
    const json encoded = json::parse(encodeChatCompletionsRequest(fixture.request));
    CHECK(encoded == readFixture("request_tools.json"));
}

TEST_CASE("openai codec sends reasoning_effort only when configured") {
    TestRequest fixture;
    // The canonical fixture above proves absence for the empty default; this
    // case pins presence and the exact value when set.
    fixture.request.reasoningEffort = "none";
    const json encoded = json::parse(encodeChatCompletionsRequest(fixture.request));
    REQUIRE(encoded.contains("reasoning_effort"));
    CHECK(encoded["reasoning_effort"] == "none");
}

TEST_CASE("openai codec appends an image_url message after an image-bearing tool result") {
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
                                      .images = {"UE5HREFUQQ=="}});

    ChatRequest request;
    request.model = "test-model";
    request.systemPrompt = "You are a test.";
    request.messages = {assistant, toolResult};
    request.tools = tools;
    request.maxTokens = 256;

    const json encoded = json::parse(encodeChatCompletionsRequest(request));
    CHECK(encoded == readFixture("request_image.json"));
}

TEST_CASE("openai codec encodes multiple images per result with the requested detail") {
    ChatMessage toolResult;
    toolResult.role = Role::Tool;
    toolResult.toolResults.push_back({.callId = "c1",
                                      .contentJson = R"({"status":"ok"})",
                                      .isError = false,
                                      .images = {"QQ==", "Qg=="},
                                      .imageDetail = "high"});

    ChatRequest request;
    request.model = "test-model";
    request.messages = {toolResult};

    const json encoded = json::parse(encodeChatCompletionsRequest(request));
    // Tool message first, then one follow-up user message carrying text + both images.
    const json& user = encoded["messages"][1];
    REQUIRE(user["role"] == "user");
    REQUIRE(user["content"].size() == 3);
    CHECK(user["content"][1]["image_url"]["url"] == "data:image/png;base64,QQ==");
    CHECK(user["content"][1]["image_url"]["detail"] == "high");
    CHECK(user["content"][2]["image_url"]["url"] == "data:image/png;base64,Qg==");
    CHECK(user["content"][2]["image_url"]["detail"] == "high");
}

TEST_CASE("openai codec encodes user reference images as content parts") {
    ChatMessage user;
    user.role = Role::User;
    user.text = "match this mood";
    user.userImages = {{.base64 = "QQ==", .mediaType = "image/jpeg"}};
    user.userImageDetail = "high";

    ChatRequest request;
    request.model = "test-model";
    request.messages = {user};

    const json encoded = json::parse(encodeChatCompletionsRequest(request));
    const json& content = encoded["messages"][0]["content"];
    REQUIRE(content.is_array());
    REQUIRE(content.size() == 2);
    CHECK(content[0]["text"] == "match this mood");
    CHECK(content[1]["image_url"]["url"] == "data:image/jpeg;base64,QQ==");
    CHECK(content[1]["image_url"]["detail"] == "high");

    // Image-less user messages keep the plain string content shape.
    ChatMessage plain;
    plain.role = Role::User;
    plain.text = "hi";
    request.messages = {plain};
    const json plainEncoded = json::parse(encodeChatCompletionsRequest(request));
    CHECK(plainEncoded["messages"][0]["content"].is_string());
}

TEST_CASE("openai codec decodes a plain text reply") {
    const auto message = decodeChatCompletionsResponse(fixtureText("response_text.json"));
    REQUIRE(message.has_value());
    CHECK(message->role == Role::Assistant);
    CHECK(message->text == "场景已经准备好了。");
    CHECK(message->toolCalls.empty());
    CHECK(message->stopReason == StopReason::EndTurn);
}

TEST_CASE("openai codec decodes tool calls with null content") {
    const auto message = decodeChatCompletionsResponse(fixtureText("response_tool_calls.json"));
    REQUIRE(message.has_value());
    CHECK(message->text.empty());
    REQUIRE(message->toolCalls.size() == 1);
    CHECK(message->toolCalls[0].id == "call_abc123");
    CHECK(message->toolCalls[0].name == "scene_add_entity");
    CHECK(message->toolCalls[0].argumentsJson == R"({"primitive":"sphere","size":1.0})");
    CHECK(message->stopReason == StopReason::ToolUse);
}

TEST_CASE("openai codec promotes finish_reason stop to ToolUse when calls are present") {
    const auto message = decodeChatCompletionsResponse(fixtureText("response_mixed.json"));
    REQUIRE(message.has_value());
    CHECK(message->text == "我先看一下场景。");
    REQUIRE(message->toolCalls.size() == 1);
    CHECK(message->toolCalls[0].name == "scene_list");
    CHECK(message->stopReason == StopReason::ToolUse);
}

TEST_CASE("openai codec maps finish_reason length to MaxTokens") {
    const auto message = decodeChatCompletionsResponse(fixtureText("response_length.json"));
    REQUIRE(message.has_value());
    CHECK(message->stopReason == StopReason::MaxTokens);
}

TEST_CASE("openai codec rejects garbage without throwing") {
    CHECK(!decodeChatCompletionsResponse("not json").has_value());
    CHECK(!decodeChatCompletionsResponse("[]").has_value());
    CHECK(!decodeChatCompletionsResponse("{}").has_value());
    CHECK(!decodeChatCompletionsResponse(R"({"choices":[]})").has_value());
    CHECK(!decodeChatCompletionsResponse(R"({"choices":[{"finish_reason":null}]})").has_value());
    // Null finish_reason with a valid message must not throw either.
    const auto message = decodeChatCompletionsResponse(
        R"({"choices":[{"message":{"content":"hi"},"finish_reason":null}]})");
    REQUIRE(message.has_value());
    CHECK(message->stopReason == StopReason::Other);
}
