#pragma once

#include <kumo/agent/tool_registry.h>

namespace kumo::renderer {
class ForwardRenderer;
}
namespace kumo::scene {
class Scene;
}

namespace kumo::agent {

// `renderer` may be null (CPU-only tests): tools that need GPU uploads or
// material access then report a structured error instead of touching it.
struct SceneToolContext {
    scene::Scene* scene = nullptr;
    renderer::ForwardRenderer* renderer = nullptr;
};

// Registers the seven scene tools (ADR 0028). The context is copied into the
// handlers; the scene and renderer it points to must outlive the registry.
void registerSceneTools(ToolRegistry& registry, SceneToolContext context);

} // namespace kumo::agent
