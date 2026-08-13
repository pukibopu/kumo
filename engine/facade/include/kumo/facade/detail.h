#pragma once

#include <kumo/agent/config.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/scene/scene.h>

#include <cstddef>
#include <string>
#include <vector>

namespace kumo::facade::detail {

// Pure mapping from a resolved AgentConfig to which sessions EngineRuntime::create
// would assemble and with which endpoint (the config-driven branch only; the
// --offline script path never reaches this). Split out so the decision is
// testable without a GPU device or network access.
struct SessionPlan {
    bool sceneEnabled = false;
    agent::AgentEndpoint sceneEndpoint;
    // English, log-facing; populated only when sceneEnabled is false.
    std::string sceneUnavailableReason;

    bool shaderEnabled = false;
    agent::AgentEndpoint shaderEndpoint;
    std::string shaderUnavailableReason;

    // Director pipeline roles (MC): tool-less, so their reasoning effort is
    // NOT coerced on OpenAI endpoints the way the tool-carrying sessions'
    // is. The critic additionally requires the scene session (it reviews the
    // scene session's work); the director alone degrades to a direct build.
    bool directorEnabled = false;
    agent::AgentEndpoint directorEndpoint;
    std::string directorUnavailableReason;

    bool criticEnabled = false;
    agent::AgentEndpoint criticEndpoint;
    std::string criticUnavailableReason;

    bool confirmDestructive = false;
};

SessionPlan planSessions(const agent::AgentConfig& config, bool confirmDestructiveOverride);

// Indices (into `current`/`snapshot`, both 0-based renderer material indices)
// where the two disagree, restricted to [0, min(current.size(), snapshot.size())):
// materials only ever get appended (ADR 0016), so anything past the shorter
// vector was created after the snapshot and is left alone. Split out from
// EngineRuntime::applySceneState so undo/redo's "only rebind materials whose
// textures actually changed" decision is testable without a GPU renderer.
std::vector<std::size_t> materialTextureDiffs(
    const std::vector<renderer::ForwardRenderer::MaterialTextureIndices>& current,
    const std::vector<renderer::ForwardRenderer::MaterialTextureIndices>& snapshot);

// The texture-set stamp carried by any entity already sharing `materialIndex`;
// empty when none does. A new entity joining a shared material must inherit
// the stamp: the GPU binding is per-material, so it renders textured either
// way, but without the stamp a save made after the stamped sibling's removal
// would reload it untextured.
std::string sharedTextureSet(const scene::Scene& scene, std::int32_t materialIndex);

} // namespace kumo::facade::detail
