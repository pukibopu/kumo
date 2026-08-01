#pragma once

#include <kumo/agent/tool_registry.h>
#include <kumo/scene/transform.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kumo::renderer {
class ForwardRenderer;
}
namespace kumo::scene {
class Scene;
}
namespace kumo::asset {
struct ProceduralSkyDesc;
}

namespace kumo::agent {

// Mirrors renderer::ForwardRenderer::MaterialParams in plain fields: that type
// is only forward-declared here (the header must not pull in the full
// forward_renderer.h), and a group definition must outlive any renderer.
struct GroupMaterialSpec {
    float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3]{0.0f, 0.0f, 0.0f};
};

// One validated primitive member of a scene_define_group assembly; `transform`
// is local to the group origin, composed onto each instance's transform by
// scene_instance_group.
struct GroupEntitySpec {
    std::string name;
    std::string primitive;
    float size = 1.0f;
    scene::Transform transform;
    GroupMaterialSpec material;
};

struct GroupDef {
    std::vector<GroupEntitySpec> members;
};

// `renderer` may be null (CPU-only tests): tools that need GPU uploads or
// material access then report a structured error instead of touching it.
struct SceneToolContext {
    scene::Scene* scene = nullptr;
    renderer::ForwardRenderer* renderer = nullptr;
    // Named assemblies defined via scene_define_group, keyed by name. Shared (the
    // shader_tools failureCounts pattern) so every registry built off the same
    // composition root sees the same definitions; registerSceneTools
    // default-constructs it when left null, but sharing across the scene, shader
    // and MCP registries requires the composition root to seed this before
    // registering any of them.
    std::shared_ptr<std::unordered_map<std::string, GroupDef>> groups;
    // environment_set's effector; null means the tool reports the feature
    // unsupported instead of touching anything (mirrors `renderer` being
    // null for CPU-only tests).
    std::function<bool(const asset::ProceduralSkyDesc&)> applyEnvironment;
    // scene_validate's frustum check needs an aspect ratio; null means it
    // assumes 16:9 and says so in its findings.
    std::function<std::pair<std::uint32_t, std::uint32_t>()> viewportSize;
};

// Registers only scene_list (ADR 0028): the read-only listing tool other agents
// (e.g. the shader assistant) need without the other scene-editing tools.
// The context is copied into the handler; the scene and renderer it points to
// must outlive the registry.
void registerSceneListTool(ToolRegistry& registry, SceneToolContext context);

// Registers the thirteen scene tools (ADR 0028, M6.9): scene_list plus the
// mutating/inspection tools (scene_add_entity, scene_add_entities,
// scene_remove_entity, scene_set_transform, camera_set, light_set,
// light_remove, material_set_param, scene_define_group, scene_instance_group,
// environment_set and scene_validate). The context is copied into the
// handlers; the scene and renderer it points to must outlive the registry.
void registerSceneTools(ToolRegistry& registry, SceneToolContext context);

} // namespace kumo::agent
