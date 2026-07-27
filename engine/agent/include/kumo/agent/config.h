#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace kumo::agent {

enum class ProviderType { Anthropic, OpenAi };

// Resolved agent configuration (ADR 0012): everything is user-supplied, nothing
// is hardcoded. `apiKey` must never reach a log.
struct AgentConfig {
    ProviderType providerType = ProviderType::Anthropic;
    std::string baseUrl;
    std::string apiKey;
    // agents.scene.model, falling back to provider.model.
    std::string sceneModel;
    std::uint32_t maxTokens = 4096;
    std::chrono::seconds requestTimeout{120};
    bool confirmDestructive = false;

    // False leaves rendering fully functional with the chat panel disabled and
    // a configuration hint (ADR 0012).
    bool agentAvailable() const;
    // English, log-facing; the panel shows its own Chinese hint.
    std::string unavailableReason() const;
};

// Loads kumo.config.json then applies overrides: real environment beats .env
// beats the file. A missing config file is not an error (defaults apply);
// malformed JSON or a wrong-typed field is. Recognized env vars:
// KUMO_PROVIDER_TYPE ("anthropic"|"openai"), KUMO_PROVIDER_BASE_URL,
// KUMO_PROVIDER_MODEL, KUMO_PROVIDER_API_KEY, ANTHROPIC_API_KEY.
std::expected<AgentConfig, std::string> loadAgentConfig(const std::filesystem::path& configPath,
                                                        const std::filesystem::path& envPath);

} // namespace kumo::agent
