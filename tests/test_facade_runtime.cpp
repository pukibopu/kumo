#include <doctest/doctest.h>

#include <kumo/facade/detail.h>

using namespace kumo::agent;
using kumo::facade::detail::planSessions;
using kumo::facade::detail::SessionPlan;

// EngineRuntime::create() needs a GPU device, so it is out of reach for
// kumo_tests; what IS pure and CPU-only is the decision of which sessions get
// assembled from a resolved AgentConfig, and with which endpoint. That mapping
// is pinned here (ADR 0043: the config-loaded branch of EngineRuntime::create,
// not the --offline scripted path, which never touches AgentConfig at all).

TEST_CASE("planSessions: scene-only local endpoint, shader left unconfigured") {
    AgentConfig config;
    config.scene.type = ProviderType::OpenAi;
    config.scene.baseUrl = "http://127.0.0.1:11434";
    config.scene.model = "qwen2.5:14b";
    // config.shader stays default-constructed: empty model, unavailable.

    const SessionPlan plan = planSessions(config, /*confirmDestructiveOverride=*/false);

    CHECK(plan.sceneEnabled);
    CHECK(plan.sceneEndpoint.type == ProviderType::OpenAi);
    CHECK(plan.sceneEndpoint.model == "qwen2.5:14b");
    CHECK(plan.sceneEndpoint.baseUrl == "http://127.0.0.1:11434");

    CHECK(!plan.shaderEnabled);
    CHECK(plan.shaderUnavailableReason.find("model") != std::string::npos);
    CHECK(!plan.confirmDestructive);
}

TEST_CASE("planSessions: both endpoints enabled, each keeping its own provider") {
    AgentConfig config;
    config.scene.type = ProviderType::Anthropic;
    config.scene.baseUrl = "https://api.anthropic.com";
    config.scene.apiKey = "sk-scene";
    config.scene.model = "scene-model";
    config.shader.type = ProviderType::OpenAi;
    config.shader.baseUrl = "http://127.0.0.1:11434";
    config.shader.model = "shader-model";

    const SessionPlan plan = planSessions(config, /*confirmDestructiveOverride=*/false);

    CHECK(plan.sceneEnabled);
    CHECK(plan.sceneEndpoint.type == ProviderType::Anthropic);
    CHECK(plan.sceneEndpoint.model == "scene-model");
    CHECK(plan.sceneEndpoint.baseUrl == "https://api.anthropic.com");

    CHECK(plan.shaderEnabled);
    CHECK(plan.shaderEndpoint.type == ProviderType::OpenAi);
    CHECK(plan.shaderEndpoint.model == "shader-model");
    CHECK(plan.shaderEndpoint.baseUrl == "http://127.0.0.1:11434");
}

TEST_CASE("planSessions: confirmDestructive is an OR of the CLI flag and the config file") {
    AgentConfig config;
    config.confirmDestructive = false;
    CHECK(!planSessions(config, false).confirmDestructive);
    CHECK(planSessions(config, true).confirmDestructive);

    config.confirmDestructive = true;
    CHECK(planSessions(config, false).confirmDestructive);
    CHECK(planSessions(config, true).confirmDestructive);
}

TEST_CASE("planSessions: unavailable reasons distinguish missing model from missing key") {
    AgentConfig config;
    // Scene has a model but is a remote endpoint with no key.
    config.scene.type = ProviderType::Anthropic;
    config.scene.baseUrl = "https://api.anthropic.com";
    config.scene.model = "scene-model";
    // Shader has neither model nor key (fully unconfigured).

    const SessionPlan plan = planSessions(config, false);

    CHECK(!plan.sceneEnabled);
    CHECK(plan.sceneUnavailableReason.find("key") != std::string::npos);

    CHECK(!plan.shaderEnabled);
    CHECK(plan.shaderUnavailableReason.find("model") != std::string::npos);
}
