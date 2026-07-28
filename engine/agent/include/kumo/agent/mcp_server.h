#pragma once
#include <kumo/agent/tool_registry.h>
#include <optional>
#include <string>
#include <string_view>

namespace kumo::agent {

// Maps one MCP JSON-RPC message to at most one response line (ADR 0041). The
// registry must outlive the server; handleMessage is called on the thread that
// owns the tools (the viewer marshals through MainThreadQueue).
class McpServer {
public:
    McpServer(const ToolRegistry& registry, std::string serverName, std::string serverVersion);

    // `line` is one JSON-RPC message without the trailing newline. Returns the
    // serialized response, or nullopt for notifications and empty lines.
    std::optional<std::string> handleMessage(std::string_view line);

private:
    const ToolRegistry& registry_;
    std::string serverName_;
    std::string serverVersion_;
};

} // namespace kumo::agent
