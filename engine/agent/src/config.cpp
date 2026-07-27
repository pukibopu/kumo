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

} // namespace

bool AgentConfig::agentAvailable() const {
    if (sceneModel.empty()) {
        return false;
    }
    if (providerType == ProviderType::OpenAi && isLocalHost(baseUrl)) {
        return true;
    }
    return !apiKey.empty();
}

std::string AgentConfig::unavailableReason() const {
    if (sceneModel.empty()) {
        return "no model configured (set provider.model or agents.scene.model in "
               "kumo.config.json)";
    }
    if (!agentAvailable()) {
        return "no api key configured (set provider.api_key or the ANTHROPIC_API_KEY "
               "environment variable)";
    }
    return {};
}

std::expected<AgentConfig, std::string> loadAgentConfig(const std::filesystem::path& configPath,
                                                        const std::filesystem::path& envPath) {
    AgentConfig config;
    std::string providerModel;
    std::string typeText;

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
            if (!readField(provider, "type", typeText, error) ||
                !readField(provider, "base_url", config.baseUrl, error) ||
                !readField(provider, "api_key", config.apiKey, error) ||
                !readField(provider, "model", providerModel, error) ||
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
            if (agents.contains("scene") && agents["scene"].is_object()) {
                if (!readField(agents["scene"], "model", config.sceneModel, error2)) {
                    return std::unexpected("agents.scene." + error2);
                }
            }
        }
    }

    const EnvLookup env{parseDotEnv(envPath)};
    if (const auto value = env.get("KUMO_PROVIDER_TYPE")) {
        typeText = *value;
    }
    if (const auto value = env.get("KUMO_PROVIDER_BASE_URL")) {
        config.baseUrl = *value;
    }
    if (const auto value = env.get("KUMO_PROVIDER_MODEL")) {
        providerModel = *value;
    }

    if (!typeText.empty()) {
        const auto type = parseProviderType(typeText);
        if (!type.has_value()) {
            return std::unexpected(
                std::format("provider.type '{}' must be 'anthropic' or 'openai'", typeText));
        }
        config.providerType = *type;
    }

    // The key resolves tier-first across its aliases: a process-env value beats
    // anything from .env no matter which variable name carries it, and both beat
    // the file. Within a tier KUMO_PROVIDER_API_KEY wins over ANTHROPIC_API_KEY.
    std::vector<const char*> keyNames{"KUMO_PROVIDER_API_KEY"};
    if (config.providerType == ProviderType::Anthropic) {
        keyNames.push_back("ANTHROPIC_API_KEY");
    }
    [&] {
        for (const auto lookup : {&EnvLookup::fromProcess, &EnvLookup::fromDotEnv}) {
            for (const char* name : keyNames) {
                if (const auto value = (env.*lookup)(name)) {
                    config.apiKey = *value;
                    return;
                }
            }
        }
    }();
    if (config.baseUrl.empty()) {
        config.baseUrl = config.providerType == ProviderType::Anthropic
                             ? "https://api.anthropic.com"
                             : "http://127.0.0.1:11434";
    }
    if (config.sceneModel.empty()) {
        config.sceneModel = providerModel;
    }
    return config;
}

} // namespace kumo::agent
