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
        for (const char* name :
             {"KUMO_PROVIDER_TYPE", "KUMO_PROVIDER_BASE_URL", "KUMO_PROVIDER_MODEL",
              "KUMO_PROVIDER_API_KEY", "ANTHROPIC_API_KEY"}) {
            unsetenv(name);
        }
        dir = std::filesystem::temp_directory_path() / "kumo_config_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    ~ConfigSandbox() {
        for (const char* name :
             {"KUMO_PROVIDER_TYPE", "KUMO_PROVIDER_BASE_URL", "KUMO_PROVIDER_MODEL",
              "KUMO_PROVIDER_API_KEY", "ANTHROPIC_API_KEY"}) {
            unsetenv(name);
        }
        std::filesystem::remove_all(dir);
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
    CHECK(config->providerType == ProviderType::Anthropic);
    CHECK(config->baseUrl == "https://api.anthropic.com");
    CHECK(config->sceneModel.empty());
    CHECK(config->maxTokens == 4096);
    CHECK(config->requestTimeout == std::chrono::seconds(120));
    CHECK(!config->confirmDestructive);
    CHECK(config->summaryThresholdTokens == 8000);
    CHECK(!config->agentAvailable());
    CHECK(config->unavailableReason().find("model") != std::string::npos);
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
    CHECK(config->providerType == ProviderType::OpenAi);
    CHECK(config->baseUrl == "http://127.0.0.1:11434");
    CHECK(config->sceneModel == "qwen2.5:14b");
    CHECK(config->maxTokens == 2048);
    CHECK(config->requestTimeout == std::chrono::seconds(60));
    CHECK(config->confirmDestructive);
    // Local endpoints do not check keys (ADR 0012).
    CHECK(config->agentAvailable());
}

TEST_CASE("agent config requires a key for remote endpoints") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "base_url": "https://api.example.com", "model": "m" }
    })");
    auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(!config->agentAvailable());
    CHECK(config->unavailableReason().find("key") != std::string::npos);

    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "base_url": "https://api.example.com",
                      "model": "m", "api_key": "sk-remote" }
    })");
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->agentAvailable());
}

TEST_CASE("agents.scene.model overrides the provider model") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "openai", "model": "global-model" },
        "agents": { "scene": { "model": "scene-model" } }
    })");
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->sceneModel == "scene-model");
}

TEST_CASE("environment beats .env beats the config file") {
    ConfigSandbox sandbox;
    sandbox.writeConfig(R"({
        "provider": { "type": "anthropic", "model": "file-model", "api_key": "sk-file" }
    })");
    sandbox.writeEnv("ANTHROPIC_API_KEY=sk-dotenv\n# comment\nKUMO_PROVIDER_MODEL=dotenv-model\n");

    auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->apiKey == "sk-dotenv");
    CHECK(config->sceneModel == "dotenv-model");

    setenv("ANTHROPIC_API_KEY", "sk-env", 1);
    setenv("KUMO_PROVIDER_MODEL", "env-model", 1);
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->apiKey == "sk-env");
    CHECK(config->sceneModel == "env-model");

    // Tier beats name: a process-env KUMO_PROVIDER_API_KEY wins over the
    // .env-sourced ANTHROPIC_API_KEY even though the latter is more specific.
    unsetenv("ANTHROPIC_API_KEY");
    setenv("KUMO_PROVIDER_API_KEY", "sk-generic-env", 1);
    config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->apiKey == "sk-generic-env");
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

TEST_CASE("KUMO_PROVIDER_TYPE switches the protocol and default base url") {
    ConfigSandbox sandbox;
    setenv("KUMO_PROVIDER_TYPE", "openai", 1);
    setenv("KUMO_PROVIDER_MODEL", "qwen2.5:14b", 1);
    const auto config = sandbox.load();
    REQUIRE(config.has_value());
    CHECK(config->providerType == ProviderType::OpenAi);
    CHECK(config->baseUrl == "http://127.0.0.1:11434");
    CHECK(config->agentAvailable());
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

    sandbox.writeConfig(R"({ "agents": { "summary_threshold_tokens": "many" } })");
    const auto badThreshold = sandbox.load();
    REQUIRE(!badThreshold.has_value());
    CHECK(badThreshold.error().find("summary_threshold_tokens") != std::string::npos);
}
