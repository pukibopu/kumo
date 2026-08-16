#include <doctest/doctest.h>

#include <kumo/agent/config.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace kumo::agent;

namespace {

// Each case starts from a clean slate: no leftover env vars, fresh temp files.
struct ConfigSandbox {
    std::filesystem::path dir;

    ConfigSandbox() {
        clearEnv();
        dir = std::filesystem::temp_directory_path() / "kumo_config_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~ConfigSandbox() {
        clearEnv();
        std::filesystem::remove_all(dir);
    }

    static void clearEnv() {
        for (const char* name :
             {"KUMO_PROVIDER_TYPE", "KUMO_PROVIDER_BASE_URL", "KUMO_PROVIDER_MODEL",
              "KUMO_PROVIDER_API_KEY", "ANTHROPIC_API_KEY", "OPENAI_API_KEY"}) {
            unsetenv(name);
        }
    }

    std::filesystem::path configPath() const { return dir / "kumo.config.json"; }
    std::filesystem::path envPath() const { return dir / ".env"; }

    void writeConfig(const std::string& text) const { write(configPath(), text); }
    void writeEnv(const std::string& text) const { write(envPath(), text); }

    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream stream(path);
        stream << text;
    }

    std::expected<AgentConfig, std::string> load() const {
        return loadAgentConfig(configPath(), envPath());
    }
};

} // namespace

TEST_CASE("agent config defaults when no file exists") {
    ConfigSandbox sandbox;
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.type == ProviderType::Anthropic);
    CHECK(config->scene.baseUrl == "https://api.anthropic.com");
    CHECK(config->scene.model.empty());
    CHECK(config->maxTokens == 4096);
    CHECK(config->requestTimeout == std::chrono::seconds(120));
    CHECK(!config->confirmDestructive);
    CHECK(config->summaryThresholdTokens == 8000);
    CHECK(!config->scene.available());
    CHECK(config->scene.unavailableReason().find("model") != std::string::npos);
    // Shader inherits the same (empty) base as scene when nothing overrides it.
    CHECK(config->shader.type == ProviderType::Anthropic);
    CHECK(config->shader.baseUrl == "https://api.anthropic.com");
}

TEST_CASE("agent config parses an openai local setup without a key") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": {
            "type": "openai",
            "base_url": "http://127.0.0.1:11434",
            "model": "qwen2.5:14b",
            "max_tokens": 2048,
            "request_timeout_seconds": 60
        },
        "agents": { "confirm_destructive": true }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.type == ProviderType::OpenAi);
    CHECK(config->scene.baseUrl == "http://127.0.0.1:11434");
    CHECK(config->scene.model == "qwen2.5:14b");
    CHECK(config->maxTokens == 2048);
    CHECK(config->requestTimeout == std::chrono::seconds(60));
    CHECK(config->confirmDestructive);
    // Local endpoints do not check keys (ADR 0012).
    CHECK(config->scene.available());
    // No agents.shader override: it inherits the same local openai endpoint.
    CHECK(config->shader.type == ProviderType::OpenAi);
    CHECK(config->shader.model == "qwen2.5:14b");
    CHECK(config->shader.available());
}

TEST_CASE("agent config requires a key for remote endpoints") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "base_url": "https://api.example.com", "model": "m" }
    })");
    auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(!config->scene.available());
    CHECK(config->scene.unavailableReason().find("key") != std::string::npos);

    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "base_url": "https://api.example.com",
                      "model": "m", "api_key": "sk-remote" }
    })");
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.available());
}

TEST_CASE("agents.scene.model overrides the provider model") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "model": "global-model" },
        "agents": { "scene": { "model": "scene-model" } }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.model == "scene-model");
    // Shader was not overridden, so it falls back to the provider model.
    CHECK(config->shader.model == "global-model");
}

TEST_CASE("agents.shader overrides base_url/api_key/model while scene inherits provider.*") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "anthropic", "base_url": "https://api.anthropic.com",
                      "api_key": "sk-scene", "model": "scene-model" },
        "agents": {
            "shader": { "base_url": "https://api.example.com", "api_key": "sk-shader",
                        "model": "shader-model" }
        }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());

    CHECK(config->scene.type == ProviderType::Anthropic);
    CHECK(config->scene.baseUrl == "https://api.anthropic.com");
    CHECK(config->scene.apiKey == "sk-scene");
    CHECK(config->scene.model == "scene-model");

    CHECK(config->shader.type == ProviderType::Anthropic);
    CHECK(config->shader.baseUrl == "https://api.example.com");
    CHECK(config->shader.apiKey == "sk-shader");
    CHECK(config->shader.model == "shader-model");
}

TEST_CASE("per-endpoint type override falls back to that type's local default base_url") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "anthropic", "model": "scene-model" },
        "agents": { "shader": { "type": "openai", "model": "shader-model" } }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    // Scene inherits the anthropic base untouched.
    CHECK(config->scene.type == ProviderType::Anthropic);
    CHECK(config->scene.baseUrl == "https://api.anthropic.com");
    // Shader's type flips to openai; its base_url was never set for either the
    // base or the shader override, so it gets the openai local default, not
    // the anthropic base_url it would have inherited.
    CHECK(config->shader.type == ProviderType::OpenAi);
    CHECK(config->shader.baseUrl == "http://127.0.0.1:11434");
}

TEST_CASE("OPENAI_API_KEY applies only to an openai-type endpoint") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "anthropic", "model": "scene-model" },
        "agents": { "shader": { "type": "openai", "base_url": "https://api.openai.com",
                                "model": "shader-model" } }
    })");
    setenv("OPENAI_API_KEY", "sk-openai-env", 1);
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    // The anthropic scene endpoint does not recognize OPENAI_API_KEY.
    CHECK(config->scene.apiKey.empty());
    CHECK(!config->scene.available());
    // The openai shader endpoint does.
    CHECK(config->shader.apiKey == "sk-openai-env");
    CHECK(config->shader.available());
}

TEST_CASE("environment beats .env beats the config file, per endpoint") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "anthropic", "model": "file-model", "api_key": "sk-file" }
    })");
    sandbox.writeEnv("ANTHROPIC_API_KEY=sk-dotenv\n# comment\nKUMO_PROVIDER_MODEL=dotenv-model\n");

    auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.apiKey == "sk-dotenv");
    CHECK(config->scene.model == "dotenv-model");

    setenv("ANTHROPIC_API_KEY", "sk-env", 1);
    setenv("KUMO_PROVIDER_MODEL", "env-model", 1);
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.apiKey == "sk-env");
    CHECK(config->scene.model == "env-model");

    // Tier beats name: a process-env KUMO_PROVIDER_API_KEY wins over the
    // .env-sourced ANTHROPIC_API_KEY even though the latter is more specific.
    unsetenv("ANTHROPIC_API_KEY");
    setenv("KUMO_PROVIDER_API_KEY", "sk-generic-env", 1);
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.apiKey == "sk-generic-env");
    unsetenv("KUMO_PROVIDER_API_KEY");
}

TEST_CASE("agents.summary_threshold_tokens overrides the default") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "agents": { "summary_threshold_tokens": 2500 }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->summaryThresholdTokens == 2500);
}

TEST_CASE("agents.max_tool_rounds defaults to 24 and can be overridden") {
    ConfigSandbox sandbox;
    const auto defaulted = sandbox.load();
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->maxToolRounds == 24);

    sandbox.writeConfig(R"({
        "agents": { "max_tool_rounds": 40 }
    })");
    const auto overridden = sandbox.load();
    REQUIRE(overridden.has_value());
    CHECK(overridden->maxToolRounds == 40);
}

TEST_CASE("provider.reasoning_effort defaults to empty and seeds both endpoints") {
    ConfigSandbox sandbox;
    const auto defaulted = sandbox.load();
    REQUIRE(defaulted.has_value());
    CHECK(defaulted->scene.reasoningEffort.empty());
    CHECK(defaulted->shader.reasoningEffort.empty());

    sandbox.writeConfig(R"({
        "provider": { "reasoning_effort": "none" }
    })");
    const auto overridden = sandbox.load();
    REQUIRE(overridden.has_value());
    CHECK(overridden->scene.reasoningEffort == "none");
    CHECK(overridden->shader.reasoningEffort == "none");
}

TEST_CASE("agents.<agent>.reasoning_effort overrides the provider seed per endpoint") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "reasoning_effort": "none" },
        "agents": { "shader": { "reasoning_effort": "high" } }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.reasoningEffort == "none");
    CHECK(config->shader.reasoningEffort == "high");
}

TEST_CASE("agents.max_tool_rounds below 2 is rejected") {
    // The final round never executes tools, so 0 or 1 would silently disable
    // the agent rather than limit it.
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "agents": { "max_tool_rounds": 0 }
    })");
    CHECK(!sandbox.load().has_value());

    sandbox.writeConfig(R"({
        "agents": { "max_tool_rounds": 1 }
    })");
    CHECK(!sandbox.load().has_value());

    sandbox.writeConfig(R"({
        "agents": { "max_tool_rounds": 2 }
    })");
    CHECK(sandbox.load().has_value());
}

TEST_CASE("KUMO_PROVIDER_TYPE switches the protocol and default base url") {
    ConfigSandbox sandbox;
    setenv("KUMO_PROVIDER_TYPE", "openai", 1);
    setenv("KUMO_PROVIDER_MODEL", "qwen2.5:14b", 1);
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->scene.type == ProviderType::OpenAi);
    CHECK(config->scene.baseUrl == "http://127.0.0.1:11434");
    CHECK(config->scene.available());
    // The env override lands on the base before the per-agent overlay, so
    // shader (which has no override of its own) sees it too.
    CHECK(config->shader.type == ProviderType::OpenAi);
    CHECK(config->shader.baseUrl == "http://127.0.0.1:11434");
}

TEST_CASE("agent config rejects malformed input with a pointed message") {
    ConfigSandbox sandbox;
    sandbox.writeConfig("{ not json");
    CHECK(!sandbox.load().has_value());

    sandbox.writeConfig(R"({ "provider": { "max_tokens": "lots" } })");
    const auto config = sandbox.load();
    REQUIRE(!config.has_value());
    CHECK(config.error().find("max_tokens") != std::string::npos);

    sandbox.writeConfig(R"({ "provider": { "type": "grpc" } })");
    const auto badType = sandbox.load();
    REQUIRE(!badType.has_value());
    CHECK(badType.error().find("grpc") != std::string::npos);

    sandbox.writeConfig(R"({ "agents": { "shader": { "type": "grpc" } } })");
    const auto badShaderType = sandbox.load();
    REQUIRE(!badShaderType.has_value());
    CHECK(badShaderType.error().find("agents.shader.type") != std::string::npos);

    sandbox.writeConfig(R"({ "agents": { "summary_threshold_tokens": "many" } })");
    const auto badThreshold = sandbox.load();
    REQUIRE(!badThreshold.has_value());
    CHECK(badThreshold.error().find("summary_threshold_tokens") != std::string::npos);
}

TEST_CASE("agents.director and agents.critic overlay the base like scene/shader (MC)") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": {"type": "openai", "base_url": "http://127.0.0.1:11434",
                     "model": "base-model", "reasoning_effort": "none"},
        "agents": {
            "director": {"model": "director-model", "reasoning_effort": "high"},
            "critic": {"model": "critic-model"}
        }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->director.model == "director-model");
    CHECK(config->director.reasoningEffort == "high");
    CHECK(config->director.baseUrl == "http://127.0.0.1:11434");
    CHECK(config->critic.model == "critic-model");
    CHECK(config->critic.reasoningEffort == "none"); // inherits the base effort
    CHECK(config->director.available());
    // Absent blocks still inherit the base wholesale.
    CHECK(config->scene.model == "base-model");
}

TEST_CASE("a wrong-typed director field points at its own path (MC)") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({"agents": {"director": {"model": 42}}})");
    const auto config = sandbox.load();
    REQUIRE(!config.has_value());
    CHECK(config.error().find("agents.director.model") != std::string::npos);
}

TEST_CASE("retrieval endpoint overrides parse and default empty (MS)") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "retrieval": {"embedding_model": "nomic-embed-text",
                      "base_url": "http://127.0.0.1:11434",
                      "caption_model": "qwen2.5vl"}
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->embeddingModel == "nomic-embed-text");
    CHECK(config->retrievalBaseUrl == "http://127.0.0.1:11434");
    CHECK(config->retrievalApiKey.empty());
    CHECK(config->captionModel == "qwen2.5vl");

    ConfigSandbox plain;
    plain.writeConfig(R"({})");
    const auto defaults = plain.load();
    REQUIRE(defaults.has_value());
    CHECK(defaults->retrievalBaseUrl.empty());
    CHECK(defaults->captionModel.empty());
}
