#pragma once

#include <kumo/agent/config.h>

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

    bool confirmDestructive = false;
};

SessionPlan planSessions(const agent::AgentConfig& config, bool confirmDestructiveOverride);

} // namespace kumo::facade::detail
