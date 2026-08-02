#include <kumo/agent/mcp_server.h>

#include "base64.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kumo::agent {

namespace {

using nlohmann::json;

// Invalid UTF-8 must degrade to U+FFFD, never to a dump() throw that would
// silently drop a response line (mirrors scene_tools.cpp's dumpSafe).
std::string dumpSafe(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool isBlank(std::string_view text) {
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

std::string okResponse(const json& id, json result) {
    return dumpSafe(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

std::string errorResponse(const json& id, int code, std::string_view message) {
    return dumpSafe(
        json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

std::string parseErrorLine() {
    return dumpSafe(json{{"jsonrpc", "2.0"},
                         {"id", nullptr},
                         {"error", {{"code", -32700}, {"message", "parse error"}}}});
}

json buildInitializeResult(const json& request, const std::string& serverName,
                           const std::string& serverVersion) {
    std::string protocolVersion = "2025-06-18";
    const auto paramsIt = request.find("params");
    if (paramsIt != request.end() && paramsIt->is_object()) {
        const auto versionIt = paramsIt->find("protocolVersion");
        if (versionIt != paramsIt->end() && versionIt->is_string()) {
            protocolVersion = versionIt->get<std::string>();
        }
    }
    return json{{"protocolVersion", protocolVersion},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", serverName}, {"version", serverVersion}}}};
}

json buildToolsList(const ToolRegistry& registry) {
    json tools = json::array();
    for (const ToolDef& def : registry.defs()) {
        json schema = json::parse(def.parametersSchema, nullptr, false);
        if (schema.is_discarded() || !schema.is_object()) {
            // Defensive: the registry guarantees a parseable object schema.
            schema = json{{"type", "object"}};
        }
        tools.push_back(
            {{"name", def.name}, {"description", def.description}, {"inputSchema", schema}});
    }
    return json{{"tools", std::move(tools)}};
}

struct RpcError {
    int code = 0;
    std::string message;
};

std::expected<json, RpcError> buildToolsCallResult(const ToolRegistry& registry,
                                                   const json& request) {
    const auto paramsIt = request.find("params");
    const json* params =
        (paramsIt != request.end() && paramsIt->is_object()) ? &(*paramsIt) : nullptr;

    if (params == nullptr) {
        return std::unexpected(RpcError{-32602, "invalid params: name"});
    }
    const auto nameIt = params->find("name");
    if (nameIt == params->end() || !nameIt->is_string()) {
        return std::unexpected(RpcError{-32602, "invalid params: name"});
    }
    const std::string name = nameIt->get<std::string>();

    json arguments = json::object();
    const auto argumentsIt = params->find("arguments");
    if (argumentsIt != params->end()) {
        arguments = *argumentsIt;
    }

    // The registry turns unknown tools and handler failures into
    // {"status":"error",...} JSON, which is protocol payload for the model to
    // read and correct itself with, not a JSON-RPC error (ADR 0028).
    const std::string resultJson = registry.invoke(name, dumpSafe(arguments));
    const json resultParsed = json::parse(resultJson, nullptr, false);

    bool isError = false;
    if (!resultParsed.is_discarded() && resultParsed.is_object()) {
        const auto statusIt = resultParsed.find("status");
        isError = statusIt != resultParsed.end() && statusIt->is_string() && *statusIt == "error";
    }

    json content = json::array();
    content.push_back({{"type", "text"}, {"text", resultJson}});

    if (!resultParsed.is_discarded() && resultParsed.is_object()) {
        std::vector<std::string> paths;
        const auto pathsIt = resultParsed.find("image_paths");
        if (pathsIt != resultParsed.end() && pathsIt->is_array()) {
            for (const json& entry : *pathsIt) {
                if (entry.is_string()) {
                    paths.push_back(entry.get<std::string>());
                }
            }
        } else if (const auto pathIt = resultParsed.find("image_path");
                   pathIt != resultParsed.end() && pathIt->is_string()) {
            paths.push_back(pathIt->get<std::string>());
        }
        for (const std::string& path : paths) {
            if (const std::optional<std::string> encoded = detail::base64EncodeFile(path);
                encoded.has_value()) {
                content.push_back(
                    {{"type", "image"}, {"data", *encoded}, {"mimeType", "image/png"}});
            }
        }
    }

    return json{{"content", std::move(content)}, {"isError", isError}};
}

} // namespace

McpServer::McpServer(const ToolRegistry& registry, std::string serverName,
                     std::string serverVersion)
    : registry_(registry), serverName_(std::move(serverName)),
      serverVersion_(std::move(serverVersion)) {}

std::optional<std::string> McpServer::handleMessage(std::string_view line) {
    if (isBlank(line)) {
        return std::nullopt;
    }

    const json request = json::parse(line, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        return parseErrorLine();
    }

    // A JSON-RPC notification omits "id" entirely; those (including
    // notifications/initialized) get no response line at all.
    const auto idIt = request.find("id");
    if (idIt == request.end()) {
        return std::nullopt;
    }
    const json& id = *idIt;

    std::string method;
    const auto methodIt = request.find("method");
    if (methodIt != request.end() && methodIt->is_string()) {
        method = methodIt->get<std::string>();
    }

    if (method == "initialize") {
        return okResponse(id, buildInitializeResult(request, serverName_, serverVersion_));
    }
    if (method == "ping") {
        return okResponse(id, json::object());
    }
    if (method == "tools/list") {
        return okResponse(id, buildToolsList(registry_));
    }
    if (method == "tools/call") {
        const std::expected<json, RpcError> result = buildToolsCallResult(registry_, request);
        if (!result.has_value()) {
            return errorResponse(id, result.error().code, result.error().message);
        }
        return okResponse(id, *result);
    }
    return errorResponse(id, -32601, std::string("method not found: ").append(method));
}

} // namespace kumo::agent
