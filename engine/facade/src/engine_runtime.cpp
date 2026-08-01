#include <kumo/facade/detail.h>
#include <kumo/facade/engine_runtime.h>

#include <kumo/agent/config.h>
#include <kumo/agent/entity_id.h>
#include <kumo/agent/fake_provider.h>
#include <kumo/agent/http_provider.h>
#include <kumo/agent/scene_tools.h>
#include <kumo/agent/shader_tools.h>
#include <kumo/asset/asset.h>
#include <kumo/asset/primitives.h>
#include <kumo/asset/procedural_sky.h>
#include <kumo/core/file.h>
#include <kumo/core/log.h>
#include <kumo/math/math.h>
#include <kumo/renderer/ibl.h>
#include <kumo/rhi/rhi.h>
#include <kumo/scene/persistence.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kumo::facade {

namespace {

constexpr rhi::TextureFormat kSwapchainFormat = rhi::TextureFormat::BGRA8Unorm;

// English for tool-call stability; the closing instruction keeps replies in the
// user's language (ADR 0028). The craft sections raise the default output from
// tech-demo to composed scene; budgets mirror the tool-layer caps.
constexpr const char* kSceneSystemPrompt =
    "You are the scene assistant inside kumo, a physically based Metal renderer. "
    "You can only affect the scene through the provided tools; never invent tool names or "
    "entity ids. Coordinates are right-handed with Y up and the camera looking down -Z; "
    "distances are in meters, angles in degrees, colors linear. Available primitives are "
    "sphere, cube, plane, cylinder, cone, torus and capsule. plane is single-sided and has "
    "zero thickness — use it only for horizontal surfaces like ground or water; any "
    "vertical panel (sign, screen, wall, roof slab) must be a thin cube instead, or it "
    "turns invisible edge-on and from behind. "
    "Call scene_list before spatial reasoning or edits that depend on current state. "
    "Tool errors come back as JSON with status \"error\": read the message, correct the "
    "call and retry.\n\n"
    "Craft process. Before building, silently decide: theme and visual style; the single "
    "main subject; what sits in foreground, midground and background; camera position and "
    "direction; a lighting plan; a material palette. Do not narrate this plan in your "
    "reply — build it.\n\n"
    "Default scene richness. Unless the user asks for something minimal, a scene includes: "
    "a ground plane or platform; one clear main subject; two to five supporting elements; "
    "an environment chosen with environment_set to match the theme; a key light plus at "
    "least one fill or rim light; at least two clearly distinct materials; a camera framed "
    "on the subject. Every element must serve the theme or composition — never scatter "
    "filler objects.\n\n"
    "Composition. Place the subject near the frame center or a rule-of-thirds point. "
    "Compute framing from data, not intuition: take the subject's aabb_world from "
    "scene_list, aim camera_set look_at at its center, and position the camera at 1.5-2.5 "
    "times the bounding-box diagonal away, slightly above subject height looking gently "
    "down; fov_y_deg 35-60. Keep the subject fully inside the frame with margin.\n\n"
    "Lighting. The first directional light is the sun and the only shadow caster — keep it "
    "at index 0 and align its direction with the sun_direction you gave environment_set. "
    "Build layered light: a key (the directional, intensity 2-5 outdoors), a fill from "
    "roughly the opposite side at 1/4-1/3 of key intensity with contrasting color "
    "temperature (point lights fall off by inverse square, so intensities 10-100 near a "
    "subject are normal), and optionally a rim light behind the subject to separate it "
    "from the background. Never leave every light pure white; pick temperatures that match "
    "the environment (warm key and cool fill at sunset, cool moonlight and warm firelight "
    "at night). Avoid lighting every side evenly. Dark themes must stay readable: keep the "
    "subject clearly brighter than the background and the silhouette separated from the "
    "sky — raise environment_set exposure or add a dim fill rather than deliver a frame "
    "that reads mostly black.\n\n"
    "Materials. Give every entity an explicit material. metallic is 0 or 1 — intermediate "
    "values are physically meaningless. roughness by intent: polished 0.05-0.15, satin "
    "0.3-0.5, matte 0.7-0.9. Use emissive only for things that emit light (fire, lamps, "
    "neon, screens) and add a matching point light at the same spot. Vary base colors "
    "deliberately across the scene and avoid saturated pure primaries — real surfaces are "
    "mixed. For effects factors cannot express — iridescence, patterns, glass, procedural "
    "texture — tell the user to ask the shader assistant.\n\n"
    "Placement. Rest objects on their support: the AABB bottom sits on the ground or the "
    "surface below, sunk 1-2 cm so nothing floats. Elevated objects need visible support — "
    "build the structure before its attachments (a sign mounts on a building wall, a lamp "
    "head on its pole, a roof on walls); never leave attachments hanging in air. No "
    "unintended interpenetration. For "
    "repeated structures define a group once with scene_define_group and stamp it with "
    "scene_instance_group scatter (count/area/seed); for several distinct entities use one "
    "scene_add_entities call (up to 128) instead of repeated single adds.\n\n"
    "Verify. After building, call scene_validate and fix warnings (floating objects, "
    "subject out of frame, unlit scenes), then check the result with scene_list. Only then "
    "reply.\n\n"
    "Budget. Default at most about 80 entities and 6 lights; the light array caps at 16. "
    "When the user asks for something bigger, prefer groups and scatter over long entity "
    "lists. Keep replies short. Always reply in the user's language.";

// English for tool-call stability; the closing instruction keeps replies in the
// user's language (ADR 0028). The binding contract mirrors docs/shaders.md and
// is the first defense against interface-breaking edits (ADR 0029).
constexpr const char* kShaderSystemPrompt =
    "You are the shader assistant inside kumo, a physically based Metal renderer whose "
    "shaders are written in Vulkan-dialect GLSL 4.60 and cross-compiled to MSL. "
    "You edit per-material fragment shaders: use scene_list to find the entity, "
    "shader_read to get the CURRENT full source of its material's fragment shader, and "
    "shader_write to replace the FULL file. Only that entity's material is affected. "
    "Binding contract you must preserve exactly: set 0 and set 2 declarations must stay "
    "byte-identical to the template (set 0 binding 0 is the FrameUniforms block included "
    "via include/common.glsl; set 2 is IBL). The push_constant block (mat4 model, mat4 "
    "normalMatrix) must not change. In set 1, bindings 0-5 (textures and sampler) must "
    "stay as-is; you MAY extend the MaterialFactors uniform block (set 1 binding 6) by "
    "appending new members at the END of the block — existing members baseColor, "
    "metallicRoughness and emissive are written by the engine, appended members read as "
    "zero until driven. Rendering is linear-light with reversed-Z, right-handed Y-up. "
    "Compile errors return structured with file and line; fix the source and retry, at "
    "most 5 attempts, then stop and explain. Keep the existing lighting structure unless "
    "asked otherwise.\n\n"
    "Material intent guide. Translate named intents into factors plus technique: brushed "
    "metal — metallic 1, roughness 0.3-0.45, tangent-aligned streak noise on roughness; "
    "polished ceramic — metallic 0, roughness 0.05-0.12, strong fresnel rim; rough "
    "concrete — metallic 0, roughness 0.85-0.95, low-frequency value noise on baseColor; "
    "frosted glass or translucent plastic — no blending exists in this pipeline, "
    "approximate with metallic 0, roughness 0.2-0.4 and fresnel-driven brightening toward "
    "grazing angles; emissive neon — emissive far above 1 with a dark baseColor; wet "
    "stone — dark baseColor, roughness patched between 0.1 and 0.6; fabric — roughness "
    "0.9 with a soft inverted-fresnel rim; painted wood — metallic 0, roughness 0.4-0.6, "
    "subtle stripe noise. Procedural detail comes from math on vUv or vWorldPos (hash "
    "noise, fract patterns) — there are no extra textures.\n\n"
    "Division of labor: the scene assistant owns placement, lighting and plain PBR "
    "factors; you own everything factors cannot express. To receive sun shadows in a "
    "custom shader, include \"shadow.glsl\" and multiply the shadow-casting light's "
    "radiance by kumoShadowPcf(vWorldPos, N) (N = the shader's world-space normal) the way "
    "pbr.frag does. "
    "Always reply in the user's language.";

// Exercises the incremental upload path: meshes and materials created after
// loadScene, with no glTF source behind them.
void addDemoPrimitives(renderer::ForwardRenderer& renderer, scene::Scene& world) {
    using MaterialParams = renderer::ForwardRenderer::MaterialParams;
    const struct {
        const char* name;
        asset::MeshData mesh;
        MaterialParams material;
        math::float3 position;
    } demos[] = {
        {"demo_sphere",
         asset::makeSphere(0.5f, 48, 24),
         {.baseColor = {1.0f, 0.77f, 0.34f, 1.0f}, .metallic = 1.0f, .roughness = 0.25f},
         {-1.5f, 0.0f, 0.0f}},
        {"demo_cube",
         asset::makeCube(0.4f),
         {.baseColor = {0.8f, 0.16f, 0.12f, 1.0f}, .metallic = 0.0f, .roughness = 0.7f},
         {1.5f, 0.0f, 0.0f}},
        {"demo_plane",
         asset::makePlane(2.0f, 4),
         {.baseColor = {0.5f, 0.5f, 0.52f, 1.0f}, .metallic = 0.0f, .roughness = 0.8f},
         {0.0f, -0.8f, 0.0f}},
    };
    for (const auto& demo : demos) {
        const std::int32_t meshIndex = renderer.addMesh(demo.mesh);
        const std::int32_t materialIndex = renderer.addMaterial(demo.material);
        if (meshIndex < 0 || materialIndex < 0) {
            logError("failed to upload demo primitive {}", demo.name);
            continue;
        }
        scene::Entity entity;
        entity.name = demo.name;
        entity.transform.position = demo.position;
        entity.meshIndex = meshIndex;
        entity.materialIndex = materialIndex;
        world.entities.insert(entity);
    }
}

// Scripted replay for `viewer --offline`: the first chat message runs the build
// demo, the second runs the destructive-delete demo (confirmation flow with
// --confirm-destructive). Assistant text is Chinese because it is user-facing.
std::vector<agent::ChatMessage> makeOfflineScript(const scene::Scene& world) {
    // Nothing has been removed yet, so the next SlotMap insert lands at
    // slots_.size() == size(): the id of the sphere the script adds is known.
    const std::string sphereId = std::format("{}:0", world.entities.size());
    auto toolStep = [](const char* text, const char* callId, const char* tool, std::string args) {
        agent::ChatMessage message;
        message.role = agent::Role::Assistant;
        message.text = text;
        message.stopReason = agent::StopReason::ToolUse;
        message.toolCalls.push_back({callId, tool, std::move(args)});
        return message;
    };
    auto say = [](const char* text) {
        agent::ChatMessage message;
        message.role = agent::Role::Assistant;
        message.text = text;
        message.stopReason = agent::StopReason::EndTurn;
        return message;
    };

    std::vector<agent::ChatMessage> script;
    script.push_back(toolStep(
        "先加一个金属球。", "call_1", "scene_add_entity",
        R"({"name":"agent_sphere","primitive":"sphere","size":1.0,"position":[0.0,1.4,0.0],)"
        R"("material":{"base_color":[1.0,0.85,0.4,1.0],"metallic":1.0,"roughness":0.15}})"));
    script.push_back(
        toolStep("把球移到头盔旁边。", "call_2", "scene_set_transform",
                 std::format(R"({{"entity_id":"{}","position":[1.3,0.4,0.3]}})", sphereId)));
    script.push_back(toolStep("改成红色粗糙材质。", "call_3", "material_set_param",
                              std::format(R"({{"entity_id":"{}","base_color":[0.75,0.08,0.06,1.0],)"
                                          R"("metallic":0.1,"roughness":0.65}})",
                                          sphereId)));
    script.push_back(toolStep("拉远相机看全景。", "call_4", "camera_set",
                              R"({"position":[0.0,1.2,4.5],"look_at":[0.4,0.2,0.0]})"));
    script.push_back(toolStep(
        "把主光调亮调暖。", "call_5", "light_set",
        R"({"index":0,"intensity":5.0,"color":[1.0,0.88,0.72],"direction":[-0.4,-0.7,-0.5]})"));
    script.push_back(say("离线演示完成：金属球已创建、移动，材质改为红色粗糙，相机拉远，主光调暖。"
                         "再发一条消息可演示删除实体（配合 --confirm-destructive 弹确认框）。"));
    script.push_back(toolStep("删除演示球体。", "call_6", "scene_remove_entity",
                              std::format(R"({{"entity_id":"{}"}})", sphereId)));
    script.push_back(say("演示球体已删除，离线脚本结束。"));
    return script;
}

using MaterialParams = renderer::ForwardRenderer::MaterialParams;

scene::SavedMaterial toSavedMaterial(const MaterialParams& params) {
    scene::SavedMaterial saved;
    std::copy(std::begin(params.baseColor), std::end(params.baseColor), saved.baseColor);
    saved.metallic = params.metallic;
    saved.roughness = params.roughness;
    std::copy(std::begin(params.emissive), std::end(params.emissive), saved.emissive);
    return saved;
}

MaterialParams toMaterialParams(const scene::SavedMaterial& saved) {
    MaterialParams params;
    std::copy(std::begin(saved.baseColor), std::end(saved.baseColor), params.baseColor);
    params.metallic = saved.metallic;
    params.roughness = saved.roughness;
    std::copy(std::begin(saved.emissive), std::end(saved.emissive), params.emissive);
    return params;
}

scene::SavedEnvironment toSavedEnvironment(const asset::ProceduralSkyDesc& desc) {
    scene::SavedEnvironment saved;
    const auto copy3 = [](const math::float3& v, float(&out)[3]) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    };
    copy3(desc.zenithColor, saved.zenithColor);
    copy3(desc.horizonColor, saved.horizonColor);
    copy3(desc.groundColor, saved.groundColor);
    copy3(desc.sunDirection, saved.sunDirection);
    copy3(desc.sunColor, saved.sunColor);
    saved.sunIntensity = desc.sunIntensity;
    saved.sunAngularRadiusDeg = desc.sunAngularRadiusDeg;
    saved.exposure = desc.exposure;
    return saved;
}

asset::ProceduralSkyDesc toProceduralSkyDesc(const scene::SavedEnvironment& saved) {
    asset::ProceduralSkyDesc desc;
    desc.zenithColor = {saved.zenithColor[0], saved.zenithColor[1], saved.zenithColor[2]};
    desc.horizonColor = {saved.horizonColor[0], saved.horizonColor[1], saved.horizonColor[2]};
    desc.groundColor = {saved.groundColor[0], saved.groundColor[1], saved.groundColor[2]};
    desc.sunDirection = {saved.sunDirection[0], saved.sunDirection[1], saved.sunDirection[2]};
    desc.sunColor = {saved.sunColor[0], saved.sunColor[1], saved.sunColor[2]};
    desc.sunIntensity = saved.sunIntensity;
    desc.sunAngularRadiusDeg = saved.sunAngularRadiusDeg;
    desc.exposure = saved.exposure;
    return desc;
}

// Tool names the undo hook must not record a checkpoint for (ADR 0044): pure
// reads, so they never change scene/renderer state, plus scene_define_group
// (M6.9), which only stores a validated assembly and never touches the scene.
// environment_set is NOT here: it mutates environmentSky_/the renderer and
// gets its undo checkpoint from this same hook like every other scene tool.
constexpr std::array<std::string_view, 5> kReadOnlyTools{
    "scene_list", "shader_read", "viewer_screenshot", "scene_define_group", "scene_validate"};

bool isFinite3(const math::float3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool isFiniteMaterial(const MaterialParams& params) {
    for (float v : params.baseColor) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    for (float v : params.emissive) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return std::isfinite(params.metallic) && std::isfinite(params.roughness);
}

// Builds an HTTP provider (OpenAI or Anthropic codec, by `endpoint.type`) wired
// to `notice` for retry feedback; shared by the scene and shader sessions so
// each gets its own transport and backoff state.
std::unique_ptr<agent::ILLMProvider> makeHttpProvider(const agent::AgentEndpoint& endpoint,
                                                      std::chrono::seconds requestTimeout,
                                                      EngineRuntime::Notice& notice) {
    agent::HttpLLMProvider::Options options;
    options.requestTimeout = requestTimeout;
    options.onRetry = [&notice](int attempt, int maxRetries) {
        notice.set(std::format("网络波动，重试中 ({}/{})…", attempt, maxRetries));
    };
    if (endpoint.type == agent::ProviderType::OpenAi) {
        return std::make_unique<agent::OpenAiProvider>(endpoint.baseUrl, endpoint.apiKey,
                                                       agent::makeUrlSessionTransport(),
                                                       std::move(options));
    }
    return std::make_unique<agent::ClaudeProvider>(
        endpoint.baseUrl, endpoint.apiKey, agent::makeUrlSessionTransport(), std::move(options));
}

} // namespace

namespace detail {

SessionPlan planSessions(const agent::AgentConfig& config, bool confirmDestructiveOverride) {
    SessionPlan plan;
    plan.confirmDestructive = confirmDestructiveOverride || config.confirmDestructive;

    plan.sceneEnabled = config.scene.available();
    if (plan.sceneEnabled) {
        plan.sceneEndpoint = config.scene;
    } else {
        plan.sceneUnavailableReason = config.scene.unavailableReason();
    }

    plan.shaderEnabled = config.shader.available();
    if (plan.shaderEnabled) {
        plan.shaderEndpoint = config.shader;
    } else {
        plan.shaderUnavailableReason = config.shader.unavailableReason();
    }

    return plan;
}

} // namespace detail

void EngineRuntime::Notice::set(std::string text) {
    std::lock_guard lock(mutex_);
    text_ = std::move(text);
}

void EngineRuntime::Notice::clear() {
    std::lock_guard lock(mutex_);
    text_.clear();
}

std::string EngineRuntime::Notice::get() const {
    std::lock_guard lock(mutex_);
    return text_;
}

EngineRuntime::EngineRuntime()
    : undo_([this] { return captureSceneState(); },
            [this](const SceneState& state) { applySceneState(state); }) {}

SceneState EngineRuntime::captureSceneState() const {
    SceneState state;
    state.world = world_;
    state.environment = environmentSky_;
    const std::uint32_t count = renderer_.materialCount();
    state.materials.reserve(count);
    state.shaderSources.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const MaterialParams* params = renderer_.materialParams(i);
        state.materials.push_back(params != nullptr ? *params : MaterialParams{});
        const std::string* source = renderer_.materialShaderSource(i);
        state.shaderSources.push_back(source != nullptr ? std::make_optional(*source)
                                                        : std::nullopt);
    }
    return state;
}

// Sizes can differ when the current renderer has MORE materials than the
// snapshot (materials are only ever appended); extra materials keep their
// current params untouched. Vectors are never shrunk (ADR 0016: GPU resources
// for removed entities/materials are never reclaimed).
void EngineRuntime::applySceneState(const SceneState& state) {
    world_ = state.world;
    const std::uint32_t currentCount = renderer_.materialCount();
    const std::size_t n = std::min<std::size_t>(state.materials.size(), currentCount);
    for (std::size_t i = 0; i < n; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        renderer_.setMaterialParams(index, state.materials[i]);
        const std::string* current = renderer_.materialShaderSource(index);
        const std::optional<std::string>& snapshotSource = state.shaderSources[i];
        if (snapshotSource.has_value()) {
            if (current == nullptr || *current != *snapshotSource) {
                // Previously-installed source: a recompile failure keeps the
                // current pipeline untouched, which is the best available
                // outcome here.
                (void)renderer_.setMaterialShader(index, *snapshotSource);
            }
        } else if (current != nullptr) {
            renderer_.clearMaterialShader(index);
        }
    }

    // Equality-gated (ProceduralSkyDesc::operator== is defaulted) so undoing
    // an unrelated edit never triggers a bake; nullopt means "restore the
    // loaded HDR", which is not otherwise reachable once a procedural sky has
    // been applied. On a bake/swap failure environmentSky_ deliberately keeps
    // tracking what the renderer actually shows — later captures then pair the
    // rolled-back world with the environment still on screen, never with a
    // value that was requested but never applied.
    if (state.environment != environmentSky_) {
        if (state.environment.has_value()) {
            const asset::HdrImage image = asset::proceduralSky(*state.environment);
            const renderer::ibl::Environment environment = renderer::ibl::bake(*device_, image);
            if (environment.valid() && renderer_.setEnvironment(environment)) {
                environmentSky_ = state.environment;
            } else {
                logError("undo/redo: failed to re-bake the procedural sky environment");
            }
        } else {
            const auto hdr = asset::loadHdr(envPath_);
            if (!hdr.has_value()) {
                logError("undo/redo: failed to reload {}: {}", envPath_.string(), hdr.error());
            } else {
                const renderer::ibl::Environment environment = renderer::ibl::bake(*device_, *hdr);
                if (environment.valid() && renderer_.setEnvironment(environment)) {
                    environmentSky_ = std::nullopt;
                } else {
                    logError("undo/redo: failed to re-bake the loaded HDR environment");
                }
            }
        }
    }
}

std::unique_ptr<EngineRuntime> EngineRuntime::create(rhi::Device& device, const Desc& desc) {
    auto sceneAsset = asset::loadGltf(desc.modelPath);
    if (!sceneAsset) {
        logError("{}", sceneAsset.error());
        return nullptr;
    }
    auto hdr = asset::loadHdr(desc.envPath);
    if (!hdr) {
        logError("{}", hdr.error());
        return nullptr;
    }
    logInfo("loaded {}: {} meshes, {} materials, {} textures, {} nodes",
            desc.modelPath.filename().string(), sceneAsset->meshes.size(),
            sceneAsset->materials.size(), sceneAsset->textures.size(), sceneAsset->nodes.size());

    auto self = std::unique_ptr<EngineRuntime>(new EngineRuntime());
    self->device_ = &device;
    self->modelPath_ = desc.modelPath;
    self->envPath_ = desc.envPath;

    if (!self->renderer_.init(device, kSwapchainFormat)) {
        return nullptr;
    }
    const renderer::ibl::Environment environment = renderer::ibl::bake(device, *hdr);
    if (!environment.valid()) {
        return nullptr;
    }
    if (!self->renderer_.loadScene(*sceneAsset, environment)) {
        return nullptr;
    }

    for (const asset::NodeInstance& node : sceneAsset->nodes) {
        if (node.meshIndex < 0) {
            continue;
        }
        const math::Trs trs = math::decomposeTrs(node.worldTransform);
        scene::Entity entity;
        entity.name = node.name;
        entity.transform = {trs.translation, trs.rotation, trs.scale};
        entity.meshIndex = node.meshIndex;
        entity.materialIndex =
            sceneAsset->meshes[static_cast<std::size_t>(node.meshIndex)].materialIndex;
        self->world_.entities.insert(entity);
    }
    self->world_.addLight({.type = scene::LightType::Directional});
    if (desc.demoPrimitives) {
        addDemoPrimitives(self->renderer_, self->world_);
    }
    // Mirrors the shells' pre-loop apply: the orbit camera's own defaults
    // become the initial view instead of scene::Camera's raw defaults.
    self->orbit_.apply(self->world_.camera);

    // Agent stack. Two registries scope each session to its own tools: the
    // shader assistant gets scene_list (to find entities) plus the shader
    // tools, but not the scene-editing tools. `groups` is seeded once here and
    // reused (like shaderTools.failureCounts below) so scene_define_group /
    // scene_instance_group definitions are visible from every registry built
    // off this runtime, chat and MCP alike.
    agent::SceneToolContext sceneTools;
    sceneTools.scene = &self->world_;
    sceneTools.renderer = &self->renderer_;
    sceneTools.groups = std::make_shared<std::unordered_map<std::string, agent::GroupDef>>();
    sceneTools.applyEnvironment = [runtime = self.get()](const asset::ProceduralSkyDesc& skyDesc) {
        return runtime->applyEnvironment(skyDesc);
    };
    sceneTools.viewportSize = [runtime = self.get()] {
        return std::make_pair(runtime->extent_.width, runtime->extent_.height);
    };
    agent::registerSceneTools(self->sceneToolRegistry_, sceneTools);
    agent::registerSceneListTool(self->shaderToolRegistry_, sceneTools);
    agent::ShaderToolContext shaderTools;
    shaderTools.scene = &self->world_;
    shaderTools.setShader = [renderer = &self->renderer_](std::uint32_t index,
                                                          std::string_view source) {
        return renderer->setMaterialShader(index, source);
    };
    shaderTools.shaderSource = [renderer = &self->renderer_](std::uint32_t index) {
        return renderer->materialShaderSource(index);
    };
    shaderTools.templatePath = desc.shaderDir / "pbr.frag";
    shaderTools.generatedDir = desc.shaderDir / "generated";
    self->generatedShaderDir_ = shaderTools.generatedDir;
    // Shared across the chat registry and the MCP registry below, so the
    // 5-attempt shader_write cap is a single per-material counter regardless
    // of which caller is driving it.
    shaderTools.failureCounts = std::make_shared<std::unordered_map<std::int32_t, int>>();
    agent::registerShaderTools(self->shaderToolRegistry_, shaderTools);

    // The MCP registry exposes the full scene tool set (unlike the shader
    // assistant's registry above, which only gets scene_list) plus the shader
    // tools and a screenshot tool for visual verification (ADR 0041). Tool
    // semantics stay single-sourced: the same contexts back both this
    // registry and the embedded assistants' registries above.
    if (desc.mcp) {
        agent::registerSceneTools(self->mcpToolRegistry_, sceneTools);
        agent::registerShaderTools(self->mcpToolRegistry_, shaderTools);
        self->mcpToolRegistry_.add(
            {.name = "viewer_screenshot",
             .description = "Render the current scene offscreen and save it as a PNG; the "
                            "image is attached to the result.",
             .parametersSchema = R"({"type":"object","properties":{}})",
             .destructive = false},
            [runtime = self.get()](std::string_view) -> std::string {
                const rhi::Extent2D extent = runtime->extent_;
                rhi::Ptr<rhi::Texture> target = runtime->device_->createTexture({
                    .size = extent,
                    .format = kSwapchainFormat,
                    .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                });
                if (!target) {
                    return agent::errorJson("failed to create offscreen render target");
                }
                rhi::Ptr<rhi::CommandEncoder> encoder =
                    runtime->device_->queue().createCommandEncoder();
                runtime->renderer_.render(*encoder, runtime->world_, target.get());
                encoder->finishAndSubmit(nullptr);
                runtime->device_->queue().waitIdle();

                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) *
                                                 extent.height * 4);
                if (!runtime->device_->queue().readTexture(
                        *target, pixels.data(), static_cast<std::uint64_t>(extent.width) * 4,
                        extent)) {
                    return agent::errorJson("screenshot readback failed");
                }
                // Swapchain is BGRA; PNG wants RGBA.
                for (std::size_t i = 0; i < pixels.size(); i += 4) {
                    std::swap(pixels[i], pixels[i + 2]);
                }
                const std::filesystem::path path =
                    std::filesystem::current_path() / "mcp_screenshot.png";
                if (!asset::writePng(path, extent.width, extent.height, pixels.data())) {
                    return agent::errorJson(
                        std::format("screenshot write failed: {}", path.string()));
                }
                return nlohmann::json{{"status", "ok"},
                                      {"path", path.string()},
                                      {"image_path", path.string()},
                                      {"width", extent.width},
                                      {"height", extent.height}}
                    .dump();
            });
    }

    // Undo capture (ADR 0044): fires for every tool invocation on all three
    // registries (chat sessions and, when enabled, MCP), so agent-driven
    // changes are undoable regardless of which caller issued them. Read-only
    // tools are excluded so they never open a pending checkpoint. The pending
    // point opened here is only ever resolved by the paired AfterInvoke hook
    // below, once the handler's result is known: BeforeInvoke and AfterInvoke
    // always fire in the same invoke() call, so no gesture is left dangling
    // across separate tool calls.
    {
        agent::ToolRegistry::BeforeInvoke before = [runtime = self.get()](std::string_view name) {
            if (std::find(kReadOnlyTools.begin(), kReadOnlyTools.end(), name) !=
                kReadOnlyTools.end()) {
                return;
            }
            runtime->undo_.beginPending(std::string(name));
        };
        // Conservative on ambiguous results: only an explicit {"status":"ok"}
        // commits; a missing/non-"ok" status, a non-object, or unparseable
        // JSON all discard (error, cancelled_by_user, and unparseable results
        // must not leave a phantom undo step).
        agent::ToolRegistry::AfterInvoke after =
            [runtime = self.get()](std::string_view, std::string_view resultJson) {
                const nlohmann::json result = nlohmann::json::parse(resultJson, nullptr, false);
                const bool ok = !result.is_discarded() && result.is_object() &&
                                result.contains("status") && result["status"].is_string() &&
                                result["status"].get<std::string>() == "ok";
                if (ok) {
                    runtime->undo_.commitPending();
                } else {
                    runtime->undo_.discardPending();
                }
            };
        self->sceneToolRegistry_.setBeforeInvoke(before);
        self->shaderToolRegistry_.setBeforeInvoke(before);
        self->mcpToolRegistry_.setBeforeInvoke(before);
        self->sceneToolRegistry_.setAfterInvoke(after);
        self->shaderToolRegistry_.setAfterInvoke(after);
        self->mcpToolRegistry_.setAfterInvoke(after);
    }

    std::optional<agent::ConfirmationGate> confirmGate;
    agent::AgentSession::Desc sceneDesc;
    agent::AgentSession::Desc shaderDesc;
    bool wantConfirm = desc.confirmDestructive;
    if (desc.offline) {
        self->sceneProvider_ = std::make_unique<agent::FakeProvider>(
            makeOfflineScript(self->world_), "离线演示脚本已播放完毕。");
        sceneDesc.model = "offline";
        logInfo("offline agent script ready");
    } else if (auto config = agent::loadAgentConfig(desc.configPath, desc.envFilePath);
               !config.has_value()) {
        logError("agent config: {}", config.error());
    } else {
        const detail::SessionPlan plan = detail::planSessions(*config, desc.confirmDestructive);
        wantConfirm = plan.confirmDestructive;
        if (plan.sceneEnabled) {
            self->sceneProvider_ = makeHttpProvider(plan.sceneEndpoint, config->requestTimeout,
                                                    self->sceneRetryNotice_);
            sceneDesc.model = plan.sceneEndpoint.model;
            sceneDesc.systemPrompt = kSceneSystemPrompt;
            sceneDesc.maxTokens = config->maxTokens;
            sceneDesc.maxToolRounds = config->maxToolRounds;
            sceneDesc.summaryThresholdTokens = config->summaryThresholdTokens;
            // The key never reaches the log (ADR 0012).
            logInfo("agent provider ready: {} {} at {}",
                    plan.sceneEndpoint.type == agent::ProviderType::OpenAi ? "openai" : "anthropic",
                    plan.sceneEndpoint.model, plan.sceneEndpoint.baseUrl);
        } else {
            // Rendering stays fully functional; the panel shows a Chinese hint.
            logInfo("agent disabled: {}", plan.sceneUnavailableReason);
        }
        if (plan.shaderEnabled) {
            self->shaderProvider_ = makeHttpProvider(plan.shaderEndpoint, config->requestTimeout,
                                                     self->shaderRetryNotice_);
            shaderDesc.model = plan.shaderEndpoint.model;
            shaderDesc.systemPrompt = kShaderSystemPrompt;
            shaderDesc.maxTokens = config->maxTokens;
            shaderDesc.maxToolRounds = config->maxToolRounds;
            shaderDesc.summaryThresholdTokens = config->summaryThresholdTokens;
            logInfo("shader agent ready: {} {} at {}",
                    plan.shaderEndpoint.type == agent::ProviderType::OpenAi ? "openai"
                                                                            : "anthropic",
                    plan.shaderEndpoint.model, plan.shaderEndpoint.baseUrl);
        } else {
            logInfo("shader agent disabled: {}", plan.shaderUnavailableReason);
        }
    }
    if (wantConfirm) {
        self->confirmGate_.emplace();
    }
    if (self->sceneProvider_ != nullptr) {
        self->sceneSession_.emplace(
            *self->sceneProvider_, self->sceneToolRegistry_, self->mainQueue_,
            self->confirmGate_.has_value() ? &*self->confirmGate_ : nullptr, sceneDesc);
    }
    if (self->shaderProvider_ != nullptr) {
        self->shaderSession_.emplace(
            *self->shaderProvider_, self->shaderToolRegistry_, self->mainQueue_,
            self->confirmGate_.has_value() ? &*self->confirmGate_ : nullptr, shaderDesc);
    }

    // MCP stdio pump (ADR 0041): one JSON-RPC line in, at most one line out.
    // handleMessage runs on the main thread by being posted through mainQueue,
    // same as every other tool callback, so it never races the render loop.
    // stdin is read via poll with a timeout instead of a blocking getline:
    // close() on a read-blocked fd is not a guaranteed wakeup, so shutdown
    // relies only on the stop flag and the drain loop in the destructor.
    if (desc.mcp) {
        self->mcpServer_.emplace(self->mcpToolRegistry_, "kumo", desc.appVersion);
        self->mcpReader_ = std::thread([runtime = self.get()] {
            std::string buffer;
            std::array<char, 4096> chunk;
            while (!runtime->mcpStop_.load()) {
                struct pollfd pfd = {.fd = 0, .events = POLLIN, .revents = 0};
                const int ready = ::poll(&pfd, 1, 100);
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (ready == 0) {
                    continue;
                }
                const ssize_t count = ::read(0, chunk.data(), chunk.size());
                if (count <= 0) {
                    break;
                }
                buffer.append(chunk.data(), static_cast<std::size_t>(count));
                std::size_t start = 0;
                for (std::size_t nl = buffer.find('\n', start); nl != std::string::npos;
                     nl = buffer.find('\n', start)) {
                    const std::string line = buffer.substr(start, nl - start);
                    start = nl + 1;
                    const std::string response =
                        runtime->mainQueue_
                            .post([runtime, line] {
                                return runtime->mcpServer_->handleMessage(line).value_or(
                                    std::string());
                            })
                            .get();
                    if (!response.empty()) {
                        std::cout << response << '\n' << std::flush;
                    }
                }
                buffer.erase(0, start);
            }
            runtime->mcpEof_.store(true);
            runtime->mcpReaderDone_.store(true);
        });
    }

    return self;
}

EngineRuntime::~EngineRuntime() {
    if (mcpReader_.joinable()) {
        // The reader may be waiting on a posted item the loop will never drain
        // again; keep draining until it observes the stop flag and exits.
        mcpStop_.store(true);
        while (!mcpReaderDone_.load()) {
            mainQueue_.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        mcpReader_.join();
    }
    if (device_ != nullptr) {
        device_->queue().waitIdle();
    }
}

bool EngineRuntime::pump() {
    // Tool callbacks land here, outside any command encoder lifetime, so scene
    // changes are frame-atomic (ADR 0005). Any drained item is a state change
    // an agent/MCP caller made off the render loop; the product shell needs a
    // frame to pick it up.
    if (mainQueue_.drain() > 0) {
        markDirty();
    }
    // Orbit arbitration (ADR 0039): user input wins and is written onto the
    // scene camera; otherwise the orbit camera adopts whatever the scene
    // camera holds, so an agent/MCP camera_set (or a scene load, drained
    // above) is not overwritten on the next drag.
    if (orbitMoved_) {
        orbit_.apply(world_.camera);
        orbitMoved_ = false;
    } else {
        orbit_.syncFrom(world_.camera);
    }
    // The MCP client hung up (stdin closed); the shell should shut down cleanly.
    return !mcpEof_.load();
}

void EngineRuntime::orbitRotate(float dx, float dy) {
    orbit_.rotate(dx, dy);
    orbitMoved_ = true;
    markDirty();
}

void EngineRuntime::orbitZoom(float delta) {
    orbit_.zoom(delta);
    orbitMoved_ = true;
    markDirty();
}

void EngineRuntime::render(rhi::CommandEncoder& encoder, rhi::Texture* output,
                           const renderer::ForwardRenderer::Overlay& overlay) {
    renderer_.render(encoder, world_, output, overlay);
}

void EngineRuntime::resize(rhi::Extent2D size) {
    extent_ = size;
    renderer_.resize(size);
    markDirty();
}

void EngineRuntime::markDirty() {
    frameDirty_.mark();
}

bool EngineRuntime::consumeRenderNeeded() {
    return frameDirty_.consume();
}

scene::Scene& EngineRuntime::world() {
    return world_;
}
renderer::ForwardRenderer& EngineRuntime::renderer() {
    return renderer_;
}
MainThreadQueue& EngineRuntime::queue() {
    return mainQueue_;
}

agent::AgentSession* EngineRuntime::sceneSession() {
    return sceneSession_.has_value() ? &*sceneSession_ : nullptr;
}

agent::AgentSession* EngineRuntime::shaderSession() {
    return shaderSession_.has_value() ? &*shaderSession_ : nullptr;
}

agent::ConfirmationGate* EngineRuntime::confirmGate() {
    return confirmGate_.has_value() ? &*confirmGate_ : nullptr;
}

bool EngineRuntime::reloadPipelines() {
    const bool ok = renderer_.reloadPipelines();
    if (ok) {
        markDirty();
    }
    return ok;
}

bool EngineRuntime::applyEnvironment(const asset::ProceduralSkyDesc& desc) {
    const asset::HdrImage image = asset::proceduralSky(desc);
    const renderer::ibl::Environment environment = renderer::ibl::bake(*device_, image);
    if (!environment.valid()) {
        logError("applyEnvironment: IBL bake failed");
        return false;
    }
    if (!renderer_.setEnvironment(environment)) {
        logError("applyEnvironment: renderer setEnvironment failed");
        return false;
    }
    environmentSky_ = desc;
    markDirty();
    return true;
}

EngineRuntime::Notice& EngineRuntime::sceneRetryNotice() {
    return sceneRetryNotice_;
}
EngineRuntime::Notice& EngineRuntime::shaderRetryNotice() {
    return shaderRetryNotice_;
}

bool EngineRuntime::saveScene(const std::filesystem::path& path) const {
    const scene::MaterialLookup lookup =
        [this](std::int32_t materialIndex) -> std::optional<scene::SavedMaterial> {
        if (materialIndex < 0) {
            return std::nullopt;
        }
        const MaterialParams* params =
            renderer_.materialParams(static_cast<std::uint32_t>(materialIndex));
        return params != nullptr ? std::make_optional(toSavedMaterial(*params)) : std::nullopt;
    };
    const std::optional<scene::SavedEnvironment> savedEnvironment =
        environmentSky_.has_value() ? std::make_optional(toSavedEnvironment(*environmentSky_))
                                    : std::nullopt;
    const std::string json =
        scene::saveSceneJson(world_, modelPath_.string(), lookup, savedEnvironment);
    std::ofstream out(path, std::ios::binary);
    out << json;
    if (!out) {
        logError("scene save failed: {}", path.string());
        return false;
    }
    logInfo("scene saved: {}", path.string());
    return true;
}

// Rebuilds procedural entities exactly like scene_add_entity does; glTF-sourced
// entities keep their saved mesh/material indices, validated against the
// currently loaded scene since the format does not persist mesh/texture data.
bool EngineRuntime::loadScene(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        // Expected state, not an error: nothing has been saved yet.
        logInfo("no saved scene at {} (press K to save one first)", path.string());
        return false;
    }
    const auto text = readTextFile(path);
    if (!text.has_value()) {
        logError("scene load failed: {}", text.error());
        return false;
    }
    const auto parsed = scene::parseSceneJson(*text);
    if (!parsed.has_value()) {
        logError("scene load failed: {}", parsed.error());
        return false;
    }
    const scene::SavedScene& saved = *parsed;
    if (saved.modelPath != modelPath_.string()) {
        logInfo("scene load: saved model '{}' differs from the current '{}'; proceeding anyway",
                saved.modelPath, modelPath_.string());
    }

    world_.entities.clear();
    world_.clearLights();
    for (std::size_t i = 0; i < saved.lights.size(); ++i) {
        if (!world_.addLight(saved.lights[i])) {
            logError("scene load: light budget exhausted; dropping {} of {} lights",
                     saved.lights.size() - i, saved.lights.size());
            break;
        }
    }
    world_.camera = saved.camera;

    // Absent key leaves the current environment (HDR or procedural) untouched.
    if (saved.environment.has_value()) {
        if (!applyEnvironment(toProceduralSkyDesc(*saved.environment))) {
            logError("scene load: failed to re-bake the saved procedural sky; keeping the current "
                     "environment");
        }
    }

    std::size_t loaded = 0;
    for (const scene::SavedEntity& savedEntity : saved.entities) {
        scene::Entity entity = savedEntity.entity;
        if (!entity.primitive.empty()) {
            std::optional<asset::MeshData> built =
                asset::makePrimitive(entity.primitive, entity.primitiveSize);
            if (!built.has_value()) {
                logError("scene load: unknown primitive '{}' on entity '{}', skipping",
                         entity.primitive, entity.name);
                continue;
            }
            asset::MeshData mesh = std::move(*built);
            const MaterialParams material = savedEntity.material.has_value()
                                                ? toMaterialParams(*savedEntity.material)
                                                : MaterialParams{};
            const std::int32_t meshIndex = renderer_.addMesh(mesh);
            const std::int32_t materialIndex = renderer_.addMaterial(material);
            if (meshIndex < 0 || materialIndex < 0) {
                logError("scene load: gpu upload failed for entity '{}', skipping", entity.name);
                continue;
            }
            entity.meshIndex = meshIndex;
            entity.materialIndex = materialIndex;
        } else {
            if (entity.meshIndex < 0 ||
                static_cast<std::uint32_t>(entity.meshIndex) >= renderer_.meshCount()) {
                logError("scene load: mesh index {} out of range for entity '{}', skipping",
                         entity.meshIndex, entity.name);
                continue;
            }
            if (savedEntity.material.has_value() && entity.materialIndex >= 0) {
                renderer_.setMaterialParams(static_cast<std::uint32_t>(entity.materialIndex),
                                            toMaterialParams(*savedEntity.material));
            }
        }
        world_.entities.insert(entity);
        ++loaded;
    }
    logInfo("scene loaded: {} entities, {} lights", loaded, saved.lights.size());
    markDirty();
    return true;
}

std::vector<EngineRuntime::EntityInfo> EngineRuntime::listEntities() const {
    std::vector<EntityInfo> out;
    world_.entities.forEach([&](scene::EntityId id, const scene::Entity& entity) {
        out.push_back(
            {.id = agent::formatEntityId(id), .name = entity.name, .primitive = entity.primitive});
    });
    return out;
}

EngineRuntime::EntityDetail EngineRuntime::entityDetail(const std::string& id) const {
    EntityDetail detail;
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return detail;
    }
    const scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr) {
        return detail;
    }
    detail.found = true;
    detail.id = id;
    detail.name = entity->name;
    detail.primitive = entity->primitive;
    detail.position = entity->transform.position;
    detail.eulerDeg = math::eulerDegrees(entity->transform.rotation);
    detail.scale = entity->transform.scale;
    if (entity->materialIndex >= 0) {
        const MaterialParams* params =
            renderer_.materialParams(static_cast<std::uint32_t>(entity->materialIndex));
        if (params != nullptr) {
            detail.hasMaterial = true;
            detail.material = *params;
            detail.hasCustomShader =
                renderer_.materialShaderSource(static_cast<std::uint32_t>(entity->materialIndex)) !=
                nullptr;
        }
    }
    return detail;
}

void EngineRuntime::beginEdit(const std::string& label) {
    undo_.beginPending(label);
}

bool EngineRuntime::setEntityTransform(const std::string& id, math::float3 position,
                                       math::float3 eulerDeg, math::float3 scale) {
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return false;
    }
    scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr) {
        return false;
    }
    if (!isFinite3(position) || !isFinite3(eulerDeg) || !isFinite3(scale)) {
        return false;
    }
    if (scale.x <= 1e-6f || scale.y <= 1e-6f || scale.z <= 1e-6f) {
        return false;
    }
    entity->transform.position = position;
    entity->transform.rotation = math::quatFromEulerDegrees(eulerDeg);
    entity->transform.scale = scale;
    // Harmless no-op when an earlier call in the same gesture already
    // committed the pending point opened by beginEdit.
    undo_.commitPending();
    markDirty();
    return true;
}

bool EngineRuntime::setEntityMaterial(const std::string& id, const MaterialParams& params) {
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return false;
    }
    scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr) {
        return false;
    }
    if (!isFiniteMaterial(params)) {
        return false;
    }
    if (entity->materialIndex < 0) {
        // Entities without a material render with the default record; give
        // them their own on first write so they become editable (mirrors
        // material_set_param).
        const std::int32_t newIndex = renderer_.addMaterial(params);
        if (newIndex < 0) {
            return false;
        }
        entity->materialIndex = newIndex;
        undo_.commitPending();
        markDirty();
        return true;
    }
    const bool applied =
        renderer_.setMaterialParams(static_cast<std::uint32_t>(entity->materialIndex), params);
    if (applied) {
        // Harmless no-op when an earlier call in the same gesture already
        // committed the pending point opened by beginEdit.
        undo_.commitPending();
        markDirty();
    }
    return applied;
}

std::optional<std::string> EngineRuntime::entityShaderSource(const std::string& id) const {
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr || entity->materialIndex < 0) {
        return std::nullopt;
    }
    const std::string* source =
        renderer_.materialShaderSource(static_cast<std::uint32_t>(entity->materialIndex));
    return source != nullptr ? std::make_optional(*source) : std::nullopt;
}

bool EngineRuntime::clearEntityShader(const std::string& id) {
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return false;
    }
    scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr || entity->materialIndex < 0) {
        return false;
    }
    const auto materialIndex = static_cast<std::uint32_t>(entity->materialIndex);
    if (renderer_.materialShaderSource(materialIndex) == nullptr) {
        return false;
    }
    undo_.beginPending("clear_shader");
    const bool cleared = renderer_.clearMaterialShader(materialIndex);
    if (cleared) {
        undo_.commitPending();
        markDirty();
    }
    return cleared;
}

std::filesystem::path EngineRuntime::generatedShaderPath(const std::string& id) const {
    const std::optional<scene::EntityId> parsed = agent::parseEntityId(id);
    if (!parsed.has_value()) {
        return {};
    }
    const scene::Entity* entity = world_.entities.get(*parsed);
    if (entity == nullptr || entity->materialIndex < 0) {
        return {};
    }
    if (renderer_.materialShaderSource(static_cast<std::uint32_t>(entity->materialIndex)) ==
        nullptr) {
        return {};
    }
    return generatedShaderDir_ / ("material_" + std::to_string(entity->materialIndex) + ".frag");
}

bool EngineRuntime::undoAvailable() const {
    return undo_.canUndo();
}
bool EngineRuntime::redoAvailable() const {
    return undo_.canRedo();
}
std::string EngineRuntime::undoLabel() const {
    const std::string* label = undo_.undoLabel();
    return label != nullptr ? *label : std::string();
}
std::string EngineRuntime::redoLabel() const {
    const std::string* label = undo_.redoLabel();
    return label != nullptr ? *label : std::string();
}
bool EngineRuntime::undo() {
    const bool ok = undo_.undo();
    if (ok) {
        markDirty();
    }
    return ok;
}
bool EngineRuntime::redo() {
    const bool ok = undo_.redo();
    if (ok) {
        markDirty();
    }
    return ok;
}

} // namespace kumo::facade
