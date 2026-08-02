#include <doctest/doctest.h>

#include <kumo/facade/detail.h>
#include <kumo/renderer/forward_renderer.h>

#include <vector>

using namespace kumo::agent;
using kumo::facade::detail::materialTextureDiffs;
using kumo::facade::detail::planSessions;
using kumo::facade::detail::SessionPlan;
using MaterialTextureIndices = kumo::renderer::ForwardRenderer::MaterialTextureIndices;

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

TEST_CASE("planSessions: OpenAI endpoints with tools coerce reasoning effort to none") {
    AgentConfig config;
    config.scene.type = ProviderType::OpenAi;
    config.scene.baseUrl = "https://api.openai.com";
    config.scene.apiKey = "sk-x";
    config.scene.model = "gpt";
    config.scene.reasoningEffort = "high";
    config.shader.type = ProviderType::Anthropic;
    config.shader.baseUrl = "https://api.anthropic.com";
    config.shader.apiKey = "sk-y";
    config.shader.model = "claude";
    config.shader.reasoningEffort = "high";

    const SessionPlan plan = planSessions(config, false);
    CHECK(plan.sceneEndpoint.reasoningEffort == "none");
    // Anthropic ignores the field; the configured value passes through.
    CHECK(plan.shaderEndpoint.reasoningEffort == "high");
}

TEST_CASE("planSessions: an empty OpenAI reasoning effort stays empty (omitted on the wire)") {
    AgentConfig config;
    config.scene.type = ProviderType::OpenAi;
    config.scene.baseUrl = "http://127.0.0.1:11434";
    config.scene.model = "qwen2.5:14b";

    const SessionPlan plan = planSessions(config, false);
    CHECK(plan.sceneEndpoint.reasoningEffort.empty());
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

// materialTextureDiffs backs applySceneState's "only rebind materials whose
// textures actually changed" decision (ADR 0044 follow-up: texture bindings
// were previously invisible to undo/redo). No GPU renderer is needed: the
// decision only ever compares two plain MaterialTextureIndices vectors.

TEST_CASE("materialTextureDiffs: identical vectors report no diffs") {
    const std::vector<MaterialTextureIndices> a{{.baseColor = 1}, {.baseColor = 2}};
    const std::vector<MaterialTextureIndices> b = a;
    CHECK(materialTextureDiffs(a, b).empty());
}

TEST_CASE("materialTextureDiffs: reports only the indices that differ") {
    const std::vector<MaterialTextureIndices> current{
        {.baseColor = 1}, {.baseColor = 2}, {.baseColor = 3}};
    const std::vector<MaterialTextureIndices> snapshot{
        {.baseColor = 1}, {.baseColor = 20}, {.baseColor = 3}};
    const std::vector<std::size_t> diffs = materialTextureDiffs(current, snapshot);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0] == 1);
}

TEST_CASE("materialTextureDiffs: a differing field other than baseColor still counts") {
    MaterialTextureIndices snapshot;
    snapshot.normal = 5;
    const std::vector<MaterialTextureIndices> current{MaterialTextureIndices{}};
    const std::vector<MaterialTextureIndices> snapshots{snapshot};
    const std::vector<std::size_t> diffs = materialTextureDiffs(current, snapshots);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0] == 0);
}

TEST_CASE("materialTextureDiffs: extra materials past the shorter vector are left alone") {
    // Mirrors applySceneState's own min(current.size(), snapshot.size()) rule:
    // materials are only ever appended (ADR 0016), so an index past either
    // vector was created after the other side's snapshot/current state.
    const std::vector<MaterialTextureIndices> current{{.baseColor = 1}, {.baseColor = 2}};
    const std::vector<MaterialTextureIndices> snapshot{{.baseColor = 1}};
    CHECK(materialTextureDiffs(current, snapshot).empty());
    CHECK(materialTextureDiffs(snapshot, current).empty());
}

TEST_CASE("materialTextureDiffs: both empty reports no diffs") {
    CHECK(materialTextureDiffs({}, {}).empty());
}

// sharedTextureSet backs instantiateModel's provenance inheritance: a new
// entity joining an already-textured shared material must carry the stamp so
// a save made after the stamped sibling's removal still reloads it textured.
TEST_CASE("sharedTextureSet: inherits the stamp from any sharer") {
    kumo::scene::Scene scene;
    kumo::scene::Entity stamped;
    stamped.materialIndex = 5;
    stamped.textureSet = "bark";
    scene.entities.insert(stamped);
    kumo::scene::Entity plain;
    plain.materialIndex = 5;
    scene.entities.insert(plain);

    CHECK(kumo::facade::detail::sharedTextureSet(scene, 5) == "bark");
    CHECK(kumo::facade::detail::sharedTextureSet(scene, 6).empty());
    CHECK(kumo::facade::detail::sharedTextureSet(scene, -1).empty());
}
