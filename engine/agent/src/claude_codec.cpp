#include <kumo/agent/claude_codec.h>

#include <nlohmann/json.hpp>

#include <utility>

namespace kumo::agent {

namespace {

using nlohmann::json;

json parseSchema(const std::string& schema) {
    json parsed = json::parse(schema, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return json{{"type", "object"}};
    }
    return parsed;
}

json parseArguments(const std::string& argumentsJson) {
    json parsed = json::parse(argumentsJson, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return json::object();
    }
    return parsed;
}

// value() throws on present-but-null keys; decoding must never throw.
std::string stringOr(const json& j, const char* key, const char* fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}

// The Messages API takes only user/assistant turns; tool results ride in user
// messages as tool_result blocks.
const char* wireRole(Role role) {
    return role == Role::Assistant ? "assistant" : "user";
}

json toContentBlocks(const ChatMessage& message) {
    json blocks = json::array();
    if (message.role == Role::Tool) {
        for (const ToolResult& result : message.toolResults) {
            json block{{"type", "tool_result"}, {"tool_use_id", result.callId}};
            // Vision feedback loop (M6.97): an image-bearing result becomes a
            // content array (text + image); image-less results keep the plain
            // string form so today's no-image wire shape stays unchanged.
            if (result.imagePngBase64.empty()) {
                block["content"] = result.contentJson;
            } else {
                json content = json::array();
                content.push_back({{"type", "text"}, {"text", result.contentJson}});
                content.push_back({{"type", "image"},
                                   {"source",
                                    {{"type", "base64"},
                                     {"media_type", "image/png"},
                                     {"data", result.imagePngBase64}}}});
                block["content"] = std::move(content);
            }
            if (result.isError) {
                block["is_error"] = true;
            }
            blocks.push_back(std::move(block));
        }
        return blocks;
    }
    if (!message.text.empty()) {
        blocks.push_back({{"type", "text"}, {"text", message.text}});
    }
    for (const ToolCall& call : message.toolCalls) {
        blocks.push_back({{"type", "tool_use"},
                          {"id", call.id},
                          {"name", call.name},
                          {"input", parseArguments(call.argumentsJson)}});
    }
    return blocks;
}

} // namespace

std::string encodeMessagesRequest(const ChatRequest& request) {
    json body{{"model", request.model}, {"max_tokens", request.maxTokens}};
    if (!request.systemPrompt.empty()) {
        body["system"] = request.systemPrompt;
    }

    json messages = json::array();
    for (const ChatMessage& message : request.messages) {
        json blocks = toContentBlocks(message);
        if (blocks.empty()) {
            // The API rejects empty content arrays.
            continue;
        }
        const char* role = wireRole(message.role);
        // Defensive merge: consecutive same-role messages (e.g. a rolled-up
        // tool_result followed by the next user text) are one wire message.
        if (!messages.empty() && messages.back()["role"] == role) {
            for (json& block : blocks) {
                messages.back()["content"].push_back(std::move(block));
            }
            continue;
        }
        messages.push_back({{"role", role}, {"content", std::move(blocks)}});
    }
    body["messages"] = std::move(messages);

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const ToolDef& def : request.tools) {
            tools.push_back({{"name", def.name},
                             {"description", def.description},
                             {"input_schema", parseSchema(def.parametersSchema)}});
        }
        body["tools"] = std::move(tools);
    }
    return body.dump();
}

std::expected<ChatMessage, std::string> decodeMessagesResponse(std::string_view body) {
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected("response body is not a JSON object");
    }
    const auto content = parsed.find("content");
    if (content == parsed.end() || !content->is_array()) {
        return std::unexpected("response has no content array");
    }

    ChatMessage out;
    out.role = Role::Assistant;
    for (const json& block : *content) {
        if (!block.is_object()) {
            continue;
        }
        const std::string type = stringOr(block, "type", "");
        if (type == "text") {
            out.text += stringOr(block, "text", "");
        } else if (type == "tool_use") {
            ToolCall call;
            call.id = stringOr(block, "id", "");
            call.name = stringOr(block, "name", "");
            const auto input = block.find("input");
            call.argumentsJson =
                input != block.end() && input->is_object() ? input->dump() : std::string("{}");
            if (!call.name.empty()) {
                out.toolCalls.push_back(std::move(call));
            }
        }
    }

    const std::string stopReason = stringOr(parsed, "stop_reason", "");
    if (stopReason == "end_turn") {
        out.stopReason = StopReason::EndTurn;
    } else if (stopReason == "tool_use") {
        out.stopReason = StopReason::ToolUse;
    } else if (stopReason == "max_tokens") {
        out.stopReason = StopReason::MaxTokens;
    } else {
        out.stopReason = StopReason::Other;
    }
    return out;
}

} // namespace kumo::agent
