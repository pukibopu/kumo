#include <doctest/doctest.h>

#include <kumo/agent/mcp_server.h>
#include <kumo/agent/tool_registry.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

using namespace kumo::agent;
using nlohmann::json;

namespace {

struct Fixture {
    ToolRegistry registry;
    std::string lastEchoArgs;
    std::filesystem::path dir;
    std::filesystem::path imagePath;
    std::filesystem::path missingImagePath;

    Fixture() {
        dir = std::filesystem::temp_directory_path() / "kumo_mcp_server_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        imagePath = dir / "shot.png";
        missingImagePath = dir / "does_not_exist.png";
        std::ofstream(imagePath, std::ios::binary) << "PNGDATA";

        registry.add(
            {.name = "echo_tool",
             .description = "Echoes its arguments back.",
             .parametersSchema = R"({"type":"object","properties":{"x":{"type":"string"}}})"},
            [this](std::string_view args) {
                lastEchoArgs = args;
                // The registry already validated args as a JSON object (ADR 0028).
                const json parsed =
                    json::parse(args.empty() ? "{}" : std::string(args), nullptr, false);
                return json{{"status", "ok"}, {"echo", parsed}}.dump();
            });

        registry.add({.name = "boom_tool",
                      .description = "Always fails.",
                      .parametersSchema = R"({"type":"object","properties":{}})"},
                     [](std::string_view) { return errorJson("boom"); });

        registry.add({.name = "screenshot_tool",
                      .description = "Returns an image attachment.",
                      .parametersSchema = R"({"type":"object","properties":{}})"},
                     [this](std::string_view) {
                         return json{{"status", "ok"}, {"image_path", imagePath.string()}}.dump();
                     });

        registry.add(
            {.name = "missing_image_tool",
             .description = "Returns a path to an image that does not exist.",
             .parametersSchema = R"({"type":"object","properties":{}})"},
            [this](std::string_view) {
                return json{{"status", "ok"}, {"image_path", missingImagePath.string()}}.dump();
            });
    }

    ~Fixture() { std::filesystem::remove_all(dir); }
};

std::optional<std::string> call(McpServer& server, const json& request) {
    return server.handleMessage(request.dump());
}

json parseResponse(const std::optional<std::string>& response) {
    REQUIRE(response.has_value());
    const json parsed = json::parse(*response, nullptr, false);
    REQUIRE(parsed.is_object());
    CHECK(parsed["jsonrpc"] == "2.0");
    return parsed;
}

json parseObject(const std::string& text) {
    const json parsed = json::parse(text, nullptr, false);
    REQUIRE(parsed.is_object());
    return parsed;
}

} // namespace

TEST_CASE("McpServer completes the initialize/notifications/ping handshake") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json initRequest = {{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "initialize"},
                              {"params", {{"protocolVersion", "2024-11-05"}}}};
    const json initResponse = parseResponse(call(server, initRequest));
    const json& initResult = initResponse["result"];
    CHECK(initResult["protocolVersion"] == "2024-11-05");
    CHECK(initResult["serverInfo"]["name"] == "kumo");
    CHECK(initResult["serverInfo"]["version"] == "0.9.0");
    REQUIRE(initResult["capabilities"].contains("tools"));
    CHECK(initResult["capabilities"]["tools"].is_object());

    // No protocolVersion in params falls back to the server's own default.
    const json initNoVersion = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}};
    const json fallbackResult = parseResponse(call(server, initNoVersion))["result"];
    CHECK(fallbackResult["protocolVersion"] == "2025-06-18");

    const json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    CHECK(!call(server, notification).has_value());

    const json pingRequest = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}};
    CHECK(parseResponse(call(server, pingRequest))["result"] == json::object());
}

TEST_CASE("McpServer tools/list mirrors the registry in registration order") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    const json result = parseResponse(call(server, request))["result"];
    REQUIRE(result["tools"].is_array());
    REQUIRE(result["tools"].size() == f.registry.defs().size());
    for (std::size_t i = 0; i < f.registry.defs().size(); ++i) {
        const ToolDef& def = f.registry.defs()[i];
        const json& tool = result["tools"][i];
        CHECK(tool["name"] == def.name);
        CHECK(tool["description"] == def.description);
        REQUIRE(tool["inputSchema"].is_object());
        CHECK(tool["inputSchema"]["type"] == parseObject(def.parametersSchema)["type"]);
    }
}

TEST_CASE("McpServer tools/call happy path round-trips the tool result as text") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json arguments = {{"x", "hi"}};
    const json request = {{"jsonrpc", "2.0"},
                          {"id", "call-1"},
                          {"method", "tools/call"},
                          {"params", {{"name", "echo_tool"}, {"arguments", arguments}}}};
    const json response = parseResponse(call(server, request));
    CHECK(response["id"] == "call-1");
    const json& result = response["result"];
    REQUIRE(result["content"].is_array());
    REQUIRE(result["content"].size() == 1);
    CHECK(result["content"][0]["type"] == "text");
    CHECK(result["isError"] == false);

    const json text = parseObject(result["content"][0]["text"].get<std::string>());
    CHECK(text["status"] == "ok");
    CHECK(text["echo"] == arguments);

    // The handler must see the same arguments the client sent.
    CHECK(parseObject(f.lastEchoArgs) == arguments);
}

TEST_CASE("McpServer tools/call with omitted arguments passes an empty object to the handler") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", "echo_tool"}}}};
    parseResponse(call(server, request));
    REQUIRE(!f.lastEchoArgs.empty());
    CHECK(parseObject(f.lastEchoArgs) == json::object());
}

TEST_CASE("McpServer tools/call on an unknown tool is isError, not a JSON-RPC error") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", "does_not_exist"}}}};
    const json response = parseResponse(call(server, request));
    CHECK(!response.contains("error"));
    const json& result = response["result"];
    CHECK(result["isError"] == true);
    const json text = parseObject(result["content"][0]["text"].get<std::string>());
    CHECK(text["status"] == "error");
    CHECK(text["message"].get<std::string>().find("does_not_exist") != std::string::npos);
}

TEST_CASE("McpServer tools/call on a handler that reports status error is isError") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", "boom_tool"}}}};
    const json response = parseResponse(call(server, request));
    const json& result = response["result"];
    CHECK(result["isError"] == true);
    const json text = parseObject(result["content"][0]["text"].get<std::string>());
    CHECK(text["status"] == "error");
    CHECK(text["message"] == "boom");
}

TEST_CASE("McpServer tools/call with a missing or wrong-typed name is invalid params") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json noParams = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}};
    CHECK(parseResponse(call(server, noParams))["error"]["code"] == -32602);

    const json emptyParams = {
        {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", json::object()}};
    CHECK(parseResponse(call(server, emptyParams))["error"]["code"] == -32602);

    const json wrongTypedName = {
        {"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", 42}}}};
    CHECK(parseResponse(call(server, wrongTypedName))["error"]["code"] == -32602);
}

TEST_CASE("McpServer reports unknown methods as method-not-found only for requests") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "totally/unknown"}};
    const json response = parseResponse(call(server, request));
    CHECK(response["id"] == 7);
    CHECK(response["error"]["code"] == -32601);
    CHECK(response["error"]["message"].get<std::string>().find("totally/unknown") !=
          std::string::npos);

    // Same unknown method as a notification (no id) draws no response at all.
    const json notification = {{"jsonrpc", "2.0"}, {"method", "totally/unknown"}};
    CHECK(!call(server, notification).has_value());
}

TEST_CASE("McpServer reports malformed lines as a parse error with a null id, and ignores blanks") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const std::optional<std::string> response = server.handleMessage("not json at all {{{");
    REQUIRE(response.has_value());
    const json parsed = parseObject(*response);
    CHECK(parsed["id"] == nullptr);
    CHECK(parsed["error"]["code"] == -32700);

    CHECK(!server.handleMessage("").has_value());
    CHECK(!server.handleMessage("   \t  ").has_value());
}

TEST_CASE("McpServer echoes string and numeric ids verbatim") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json stringIdRequest = {{"jsonrpc", "2.0"}, {"id", "abc"}, {"method", "ping"}};
    const json stringIdResponse = parseResponse(call(server, stringIdRequest));
    REQUIRE(stringIdResponse["id"].is_string());
    CHECK(stringIdResponse["id"] == "abc");

    const json numericIdRequest = {{"jsonrpc", "2.0"}, {"id", 42}, {"method", "ping"}};
    const json numericIdResponse = parseResponse(call(server, numericIdRequest));
    REQUIRE(numericIdResponse["id"].is_number());
    CHECK(numericIdResponse["id"] == 42);
}

TEST_CASE("McpServer attaches image content when a tool result carries an existing image_path") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", "screenshot_tool"}}}};
    const json response = parseResponse(call(server, request));
    const json& result = response["result"];
    REQUIRE(result["content"].size() == 2);
    CHECK(result["content"][0]["type"] == "text");
    CHECK(result["content"][1]["type"] == "image");
    CHECK(result["content"][1]["mimeType"] == "image/png");
    // base64("PNGDATA"), computed independently of the encoder under test.
    CHECK(result["content"][1]["data"] == "UE5HREFUQQ==");
}

TEST_CASE("McpServer omits the image block when image_path does not exist") {
    Fixture f;
    McpServer server(f.registry, "kumo", "0.9.0");

    const json request = {{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "tools/call"},
                          {"params", {{"name", "missing_image_tool"}}}};
    const json response = parseResponse(call(server, request));
    const json& result = response["result"];
    REQUIRE(result["content"].size() == 1);
    CHECK(result["content"][0]["type"] == "text");
}
