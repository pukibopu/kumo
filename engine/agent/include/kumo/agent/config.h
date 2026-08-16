#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace kumo::agent {

enum class ProviderType { Anthropic, OpenAi };

// One provider endpoint resolved for a single agent (ADR 0012). `apiKey` must
// never reach a log.
struct AgentEndpoint {
    ProviderType type = ProviderType::Anthropic;
    std::string baseUrl;
    std::string apiKey;
    std::string model;
    // Per-agent reasoning effort; empty = omitted from requests (MB-4).
    std::string reasoningEffort;

    // False leaves that agent's chat panel disabled with a configuration hint.
    bool available() const;
    // English, log-facing; the panel shows its own Chinese hint.
    std::string unavailableReason() const;
};

// Resolved agent configuration: the scene agent and the shader agent may run
// against different providers (e.g. a local model for scene edits, a cloud
// model for shader generation), so each gets its own endpoint (ADR 0012).
struct AgentConfig {
    AgentEndpoint scene;
    AgentEndpoint shader;
    // Director pipeline roles (MC): tool-less sessions, so unlike scene/shader
    // their reasoning effort is never coerced away on OpenAI endpoints.
    AgentEndpoint director;
    AgentEndpoint critic;
    std::uint32_t maxTokens = 4096;
    std::chrono::seconds requestTimeout{120};
    bool confirmDestructive = false;
    // agents.summary_threshold_tokens; 0 disables history compression.
    std::size_t summaryThresholdTokens = 8000;
    // agents.max_tool_rounds; global like summary_threshold_tokens (no
    // per-agent override), applied to both the scene and shader sessions.
    int maxToolRounds = 24;
    // retrieval.embedding_model (MR): the /v1/embeddings model used by the
    // index builder and query-time asset_search. The endpoint comes from
    // retrieval.base_url when set (MS: a local Ollama can serve retrieval
    // while the agents stay on a cloud provider; api_key may stay empty for
    // local servers), else from whichever agent endpoint is openai-typed.
    std::string embeddingModel = "text-embedding-3-small";
    std::string retrievalBaseUrl;
    std::string retrievalApiKey;
    // retrieval.caption_model (MS): when set, `viewer --index` captions
    // thumbnails via chat completions on the retrieval endpoint with this
    // model instead of the scene/shader agent endpoint.
    std::string captionModel;
};

// Loads kumo.config.json then applies overrides: real environment beats .env
// beats the file. A missing config file is not an error (defaults apply);
// malformed JSON or a wrong-typed field is.
//
// provider.* seeds a base endpoint; agents.scene.* and agents.shader.* each
// override that base per-field (absent or empty fields inherit the base).
// KUMO_PROVIDER_TYPE/BASE_URL/MODEL override the base before the per-agent
// overlay is applied. Each endpoint's api key then resolves tier-first from
// its own type: KUMO_PROVIDER_API_KEY, then ANTHROPIC_API_KEY (Anthropic
// endpoints) or OPENAI_API_KEY (OpenAi endpoints), process env beating .env
// beating the config file. An empty base_url defaults per the endpoint's own
// resolved type (anthropic -> api.anthropic.com, openai -> local 127.0.0.1),
// applied after every override.
std::expected<AgentConfig, std::string> loadAgentConfig(const std::filesystem::path& configPath,
                                                        const std::filesystem::path& envPath);

} // namespace kumo::agent
