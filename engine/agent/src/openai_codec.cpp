#include <kumo/agent/openai_codec.h>

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

// value() throws on present-but-null keys (finish_reason: null is a thing local
// endpoints produce); decoding must never throw.
std::string stringOr(const json& j, const char* key, const char* fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}

const char* roleName(Role role) {
    switch (role) {
    case Role::System:
        return "system";
    case Role::Assistant:
        return "assistant";
    case Role::Tool:
        return "tool";
    case Role::User:
        break;
    }
    return "user";
}

} // namespace

std::string encodeChatCompletionsRequest(const ChatRequest& request) {
    // Current OpenAI models reject the legacy max_tokens name.
    json body{{"model", request.model}, {"max_completion_tokens", request.maxTokens}};
    // Only when configured: non-reasoning models and OpenAI-compatible local
    // endpoints reject unknown request fields.
    if (!request.reasoningEffort.empty()) {
        body["reasoning_effort"] = request.reasoningEffort;
    }

    json messages = json::array();
    if (!request.systemPrompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", request.systemPrompt}});
    }
    for (const ChatMessage& message : request.messages) {
        if (message.role == Role::Tool) {
            // Chat Completions carries each tool result as its own message.
            for (const ToolResult& result : message.toolResults) {
                messages.push_back({{"role", "tool"},
                                    {"tool_call_id", result.callId},
                                    {"content", result.contentJson}});
            }
            // The tool-role message has no image slot on this wire, so
            // image-bearing results are resent as one follow-up user message.
            json imageParts = json::array();
            for (const ToolResult& result : message.toolResults) {
                const std::string detail = result.imageDetail.empty() ? "low" : result.imageDetail;
                for (const std::string& image : result.images) {
                    imageParts.push_back(
                        {{"type", "image_url"},
                         {"image_url",
                          {{"url", "data:image/png;base64," + image}, {"detail", detail}}}});
                }
            }
            if (!imageParts.empty()) {
                json content = json::array();
                content.push_back(
                    {{"type", "text"}, {"text", "Screenshot from the last tool call:"}});
                for (json& part : imageParts) {
                    content.push_back(std::move(part));
                }
                messages.push_back({{"role", "user"}, {"content", std::move(content)}});
            }
            continue;
        }
        json entry{{"role", roleName(message.role)}, {"content", message.text}};
        // Reference images (MB-5): the plain-string content becomes parts.
        if (message.role == Role::User && !message.userImages.empty()) {
            const std::string detail =
                message.userImageDetail.empty() ? "low" : message.userImageDetail;
            json content = json::array();
            content.push_back({{"type", "text"}, {"text", message.text}});
            for (const UserImage& image : message.userImages) {
                content.push_back({{"type", "image_url"},
                                   {"image_url",
                                    {{"url", "data:" + image.mediaType + ";base64," + image.base64},
                                     {"detail", detail}}}});
            }
            entry["content"] = std::move(content);
        }
        if (!message.toolCalls.empty()) {
            json calls = json::array();
            for (const ToolCall& call : message.toolCalls) {
                calls.push_back(
                    {{"id", call.id},
                     {"type", "function"},
                     {"function", {{"name", call.name}, {"arguments", call.argumentsJson}}}});
            }
            entry["tool_calls"] = std::move(calls);
        }
        messages.push_back(std::move(entry));
    }
    body["messages"] = std::move(messages);

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const ToolDef& def : request.tools) {
            tools.push_back({{"type", "function"},
                             {"function",
                              {{"name", def.name},
                               {"description", def.description},
                               {"parameters", parseSchema(def.parametersSchema)}}}});
        }
        body["tools"] = std::move(tools);
    }
    return body.dump();
}

std::expected<ChatMessage, std::string> decodeChatCompletionsResponse(std::string_view body) {
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected("response body is not a JSON object");
    }
    const auto choices = parsed.find("choices");
    if (choices == parsed.end() || !choices->is_array() || choices->empty()) {
        return std::unexpected("response has no choices");
    }
    const json& choice = (*choices)[0];
    const auto messageIt = choice.find("message");
    if (messageIt == choice.end() || !messageIt->is_object()) {
        return std::unexpected("choice has no message object");
    }

    ChatMessage out;
    out.role = Role::Assistant;
    const json& message = *messageIt;
    const auto content = message.find("content");
    if (content != message.end() && content->is_string()) {
        out.text = content->get<std::string>();
    }
    const auto calls = message.find("tool_calls");
    if (calls != message.end() && calls->is_array()) {
        for (const json& call : *calls) {
            if (!call.is_object()) {
                continue;
            }
            const auto function = call.find("function");
            if (function == call.end() || !function->is_object()) {
                continue;
            }
            ToolCall toolCall;
            toolCall.id = stringOr(call, "id", "");
            toolCall.name = stringOr(*function, "name", "");
            toolCall.argumentsJson = stringOr(*function, "arguments", "{}");
            if (!toolCall.name.empty()) {
                out.toolCalls.push_back(std::move(toolCall));
            }
        }
    }

    const std::string finishReason = stringOr(choice, "finish_reason", "");
    if (finishReason == "stop") {
        out.stopReason = StopReason::EndTurn;
    } else if (finishReason == "tool_calls") {
        out.stopReason = StopReason::ToolUse;
    } else if (finishReason == "length") {
        out.stopReason = StopReason::MaxTokens;
    } else {
        out.stopReason = StopReason::Other;
    }
    // Some local endpoints report finish_reason "stop" even for tool calls.
    if (!out.toolCalls.empty() && out.stopReason == StopReason::EndTurn) {
        out.stopReason = StopReason::ToolUse;
    }
    return out;
}

} // namespace kumo::agent
