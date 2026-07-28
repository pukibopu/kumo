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

// Registers only scene_list (ADR 0028): the read-only listing tool other agents
// (e.g. the shader assistant) need without the other six scene-editing tools.
// The context is copied into the handler; the scene and renderer it points to
// must outlive the registry.
void registerSceneListTool(ToolRegistry& registry, SceneToolContext context);

// Registers the eight scene tools (ADR 0028): scene_list plus the seven
// mutating tools (scene_add_entity, scene_add_entities and five more). The
// context is copied into the handlers; the scene and renderer it points to
// must outlive the registry.
void registerSceneTools(ToolRegistry& registry, SceneToolContext context);

} // namespace kumo::agent
