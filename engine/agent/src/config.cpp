#include <kumo/agent/config.h>

#include <kumo/core/file.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kumo::agent {

namespace {

using nlohmann::json;

bool isLocalHost(std::string_view url) {
    return url.find("://localhost") != std::string_view::npos ||
           url.find("://127.0.0.1") != std::string_view::npos ||
           url.find("://[::1]") != std::string_view::npos;
}

// KEY=VALUE lines; '#' starts a comment. Values are used only as fallbacks for
// unset environment variables.
std::unordered_map<std::string, std::string> parseDotEnv(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

struct EnvLookup {
    std::unordered_map<std::string, std::string> dotEnv;

    std::optional<std::string> fromProcess(const char* name) const {
        if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
            return std::string(value);
        }
        return std::nullopt;
    }

    std::optional<std::string> fromDotEnv(const char* name) const {
        const auto it = dotEnv.find(name);
        if (it != dotEnv.end() && !it->second.empty()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<std::string> get(const char* name) const {
        if (auto value = fromProcess(name)) {
            return value;
        }
        return fromDotEnv(name);
    }
};

template <typename T>
bool readField(const json& object, const char* key, T& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) {
            error = std::format("{} must be a string", key);
            return false;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            error = std::format("{} must be a boolean", key);
            return false;
        }
    } else {
        if (!it->is_number_unsigned()) {
            error = std::format("{} must be a non-negative integer", key);
            return false;
        }
    }
    out = it->get<T>();
    return true;
}

std::optional<ProviderType> parseProviderType(std::string_view text) {
    if (text == "anthropic") {
        return ProviderType::Anthropic;
    }
    if (text == "openai") {
        return ProviderType::OpenAi;
    }
    return std::nullopt;
}

// Empty means "inherit the base"; validated independently so an override's
// error message points at its own field ("agents.shader.type", not
// "provider.type") instead of blaming whichever endpoint contributed the value.
std::expected<ProviderType, std::string> resolveType(std::string_view text, const char* field,
                                                     ProviderType fallback) {
    if (text.empty()) {
        return fallback;
    }
    const auto type = parseProviderType(text);
    if (!type.has_value()) {
        return std::unexpected(std::format("{} '{}' must be 'anthropic' or 'openai'", field, text));
    }
    return *type;
}

std::string pick(const std::string& override, const std::string& base) {
    return override.empty() ? base : override;
}

// Resolves one endpoint's api key/base_url from its already-merged raw fields
// and its own resolved type; the alias tier order and the local-default base
// url both depend on that type, so this must run after per-agent overlay.
AgentEndpoint finalizeEndpoint(ProviderType type, std::string baseUrl, std::string apiKeyConfig,
                               std::string model, const EnvLookup& env) {
    AgentEndpoint endpoint;
    endpoint.type = type;
    endpoint.model = std::move(model);

    // The key resolves tier-first across its aliases: a process-env value beats
    // anything from .env no matter which variable name carries it, and both beat
    // the config file. Within a tier KUMO_PROVIDER_API_KEY wins over the
    // type-specific alias.
    std::vector<const char*> keyNames{"KUMO_PROVIDER_API_KEY"};
    if (type == ProviderType::Anthropic) {
        keyNames.push_back("ANTHROPIC_API_KEY");
    } else {
        keyNames.push_back("OPENAI_API_KEY");
    }
    endpoint.apiKey = std::move(apiKeyConfig);
    for (const auto lookup : {&EnvLookup::fromProcess, &EnvLookup::fromDotEnv}) {
        bool found = false;
        for (const char* name : keyNames) {
            if (const auto value = (env.*lookup)(name)) {
                endpoint.apiKey = *value;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    endpoint.baseUrl = std::move(baseUrl);
    if (endpoint.baseUrl.empty()) {
        endpoint.baseUrl = type == ProviderType::Anthropic ? "https://api.anthropic.com"
                                                           : "http://127.0.0.1:11434";
    }
    return endpoint;
}

} // namespace

bool AgentEndpoint::available() const {
    if (model.empty()) {
        return false;
    }
    if (type == ProviderType::OpenAi && isLocalHost(baseUrl)) {
        return true;
    }
    return !apiKey.empty();
}

std::string AgentEndpoint::unavailableReason() const {
    if (model.empty()) {
        return "no model configured (set provider.model or agents.<agent>.model in "
               "kumo.config.json)";
    }
    if (!available()) {
        return "no api key configured (set provider.api_key, agents.<agent>.api_key, or the "
               "KUMO_PROVIDER_API_KEY/ANTHROPIC_API_KEY/OPENAI_API_KEY environment variable)";
    }
    return {};
}

std::expected<AgentConfig, std::string> loadAgentConfig(const std::filesystem::path& configPath,
                                                        const std::filesystem::path& envPath) {
    AgentConfig config;
    std::string baseTypeText;
    std::string baseUrl;
    std::string baseApiKey;
    std::string baseModel;

    std::string sceneTypeText, sceneBaseUrl, sceneApiKey, sceneModel;
    std::string shaderTypeText, shaderBaseUrl, shaderApiKey, shaderModel;

    if (std::filesystem::exists(configPath)) {
        const auto text = readTextFile(configPath);
        if (!text.has_value()) {
            return std::unexpected(std::format("cannot read {}", configPath.string()));
        }
        const json parsed = json::parse(*text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return std::unexpected(std::format("{} is not valid JSON", configPath.string()));
        }
        std::string error;
        if (parsed.contains("provider")) {
            const json& provider = parsed["provider"];
            if (!provider.is_object()) {
                return std::unexpected("provider must be an object");
            }
            std::uint32_t timeoutSeconds = 120;
            if (!readField(provider, "type", baseTypeText, error) ||
                !readField(provider, "base_url", baseUrl, error) ||
                !readField(provider, "api_key", baseApiKey, error) ||
                !readField(provider, "model", baseModel, error) ||
                !readField(provider, "max_tokens", config.maxTokens, error) ||
                !readField(provider, "request_timeout_seconds", timeoutSeconds, error)) {
                return std::unexpected("provider." + error);
            }
            config.requestTimeout = std::chrono::seconds(timeoutSeconds);
        }
        if (parsed.contains("agents")) {
            const json& agents = parsed["agents"];
            if (!agents.is_object()) {
                return std::unexpected("agents must be an object");
            }
            std::string error2;
            if (!readField(agents, "confirm_destructive", config.confirmDestructive, error2)) {
                return std::unexpected("agents." + error2);
            }
            if (!readField(agents, "summary_threshold_tokens", config.summaryThresholdTokens,
                           error2)) {
                return std::unexpected("agents." + error2);
            }
            if (!readField(agents, "max_tool_rounds", config.maxToolRounds, error2)) {
                return std::unexpected("agents." + error2);
            }
            // The final round never executes tools (session.cpp), so 0 or 1
            // would silently disable the agent instead of limiting it.
            if (config.maxToolRounds < 2) {
                return std::unexpected("agents.max_tool_rounds must be at least 2");
            }
            if (agents.contains("scene") && agents["scene"].is_object()) {
                const json& scene = agents["scene"];
                if (!readField(scene, "type", sceneTypeText, error2) ||
                    !readField(scene, "base_url", sceneBaseUrl, error2) ||
                    !readField(scene, "api_key", sceneApiKey, error2) ||
                    !readField(scene, "model", sceneModel, error2)) {
                    return std::unexpected("agents.scene." + error2);
                }
            }
            if (agents.contains("shader") && agents["shader"].is_object()) {
                const json& shader = agents["shader"];
                if (!readField(shader, "type", shaderTypeText, error2) ||
                    !readField(shader, "base_url", shaderBaseUrl, error2) ||
                    !readField(shader, "api_key", shaderApiKey, error2) ||
                    !readField(shader, "model", shaderModel, error2)) {
                    return std::unexpected("agents.shader." + error2);
                }
            }
        }
    }

    const EnvLookup env{parseDotEnv(envPath)};
    // Env overrides apply to the base before the per-agent overlay, so an
    // agents.scene/agents.shader field left unset still inherits the env value.
    if (const auto value = env.get("KUMO_PROVIDER_TYPE")) {
        baseTypeText = *value;
    }
    if (const auto value = env.get("KUMO_PROVIDER_BASE_URL")) {
        baseUrl = *value;
    }
    if (const auto value = env.get("KUMO_PROVIDER_MODEL")) {
        baseModel = *value;
    }

    const auto baseType = resolveType(baseTypeText, "provider.type", ProviderType::Anthropic);
    if (!baseType.has_value()) {
        return std::unexpected(baseType.error());
    }
    const auto sceneType = resolveType(sceneTypeText, "agents.scene.type", *baseType);
    if (!sceneType.has_value()) {
        return std::unexpected(sceneType.error());
    }
    const auto shaderType = resolveType(shaderTypeText, "agents.shader.type", *baseType);
    if (!shaderType.has_value()) {
        return std::unexpected(shaderType.error());
    }

    config.scene =
        finalizeEndpoint(*sceneType, pick(sceneBaseUrl, baseUrl), pick(sceneApiKey, baseApiKey),
                         pick(sceneModel, baseModel), env);
    config.shader =
        finalizeEndpoint(*shaderType, pick(shaderBaseUrl, baseUrl), pick(shaderApiKey, baseApiKey),
                         pick(shaderModel, baseModel), env);
    return config;
}

} // namespace kumo::agent
