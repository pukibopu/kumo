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
#include <kumo/gpu/gpu.h>
#include <kumo/math/math.h>
#include <kumo/renderer/ibl.h>
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

constexpr gpu::TextureFormat kSwapchainFormat = gpu::TextureFormat::BGRA8Unorm;

// Vision feedback loop (M6.97): keeps the screenshot attachment cheap on the
// wire regardless of viewport size.
constexpr std::uint32_t kScreenshotMaxLongSide = 640;

// Seeded into a freshly rebuilt session's transcript by reloadAgentSessions()
// (settings hot apply): the chat window otherwise goes silently empty with no
// explanation for why the earlier conversation is gone.
constexpr const char* kAgentReloadNotice = "Agent 配置已重新加载，会话历史已重置。";

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
    "Verify, then deliver. After building, call scene_validate and fix warnings (floating "
    "objects, subject out of frame, unlit scenes). Then call viewer_screenshot ONCE and "
    "study the image like an art director: framing, exposure, scale, anything clearly "
    "wrong or missing. If — and only if — you found a real defect, fix it and take one "
    "second screenshot to confirm. Never take a third. Do not chase perfection: when the "
    "scene serves the theme, stop editing and reply, mentioning any remaining small flaws "
    "in one sentence instead of fixing them. An imperfect scene delivered beats an endless "
    "adjustment loop.\n\n"
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
    "metallicRoughness, emissive and uvTiling are written by the engine, members appended "
    "after uvTiling read as zero until driven. Rendering is linear-light with reversed-Z, "
    "right-handed Y-up. "
    "Compile errors return structured with file and line; fix the source and retry, at "
    "most 5 attempts, then stop and explain. Keep the existing lighting structure unless "
    "asked otherwise. After a shader_write that compiles, call viewer_screenshot once to "
    "inspect the material in situ; if — and only if — the look is clearly wrong, fix it and "
    "confirm with at most one more screenshot. Never loop beyond that.\n\n"
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
    "frame.timeParams.x holds seconds elapsed since the renderer launched, for animation "
    "(flickering flames, flowing water, pulsing neon); using it keeps the viewport "
    "redrawing automatically. "
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
    std::copy(std::begin(params.uvTiling), std::end(params.uvTiling), saved.uvTiling);
    return saved;
}

MaterialParams toMaterialParams(const scene::SavedMaterial& saved) {
    MaterialParams params;
    std::copy(std::begin(saved.baseColor), std::end(saved.baseColor), params.baseColor);
    params.metallic = saved.metallic;
    params.roughness = saved.roughness;
    std::copy(std::begin(saved.emissive), std::end(saved.emissive), params.emissive);
    std::copy(std::begin(saved.uvTiling), std::end(saved.uvTiling), params.uvTiling);
    return params;
}

scene::SavedEnvironment toSavedEnvironment(const EnvironmentSource& source) {
    scene::SavedEnvironment saved;
    const auto copy3 = [](const math::float3& v, float(&out)[3]) {
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    };
    const asset::ProceduralSkyDesc& desc = source.sky;
    copy3(desc.zenithColor, saved.zenithColor);
    copy3(desc.horizonColor, saved.horizonColor);
    copy3(desc.groundColor, saved.groundColor);
    copy3(desc.sunDirection, saved.sunDirection);
    copy3(desc.sunColor, saved.sunColor);
    saved.sunIntensity = desc.sunIntensity;
    saved.sunAngularRadiusDeg = desc.sunAngularRadiusDeg;
    saved.exposure = desc.exposure;
    // Procedural fields above are still written even when `file` is set (kept
    // human-editable/inspectable); the loader ignores them in that case.
    if (!source.file.empty()) {
        saved.file = source.file;
    }
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

EnvironmentSource toEnvironmentSource(const scene::SavedEnvironment& saved) {
    if (saved.file.has_value() && !saved.file->empty()) {
        return EnvironmentSource{.file = *saved.file};
    }
    return EnvironmentSource{.file = "", .sky = toProceduralSkyDesc(saved)};
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

    // Equality-gated (EnvironmentSource::operator== is defaulted) so undoing
    // an unrelated edit never triggers a bake; nullopt means "restore the
    // loaded startup HDR", which is not otherwise reachable once
    // environment_set has swapped in a procedural sky or a named HDR file. On
    // a bake/swap failure environmentSky_ deliberately keeps tracking what the
    // renderer actually shows — later captures then pair the rolled-back
    // world with the environment still on screen, never with a value that was
    // requested but never applied.
    if (state.environment != environmentSky_) {
        if (state.environment.has_value()) {
            if (!applyEnvironmentSource(*state.environment)) {
                logError("undo/redo: failed to apply the saved environment");
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

// Reads configPath_/envFilePath_/offline_/confirmDestructiveOverride_ (or
// replays the --offline script) and (re)builds sceneProvider_/shaderProvider_/
// confirmGate_/sceneSession_/shaderSession_ from scratch. Assumes those five
// are already reset/empty; create() starts that way, reloadAgentSessions()
// tears them down first. `isReload` is the only difference between the two
// call sites: it seeds the freshly built sessions' transcripts with a note
// that history was just reset, since a reload never happens on session
// construction from create().
void EngineRuntime::assembleAgentSessions(bool isReload) {
    agent::AgentSession::Desc sceneDesc;
    agent::AgentSession::Desc shaderDesc;
    bool wantConfirm = confirmDestructiveOverride_;
    if (offline_) {
        sceneProvider_ = std::make_unique<agent::FakeProvider>(makeOfflineScript(world_),
                                                               "离线演示脚本已播放完毕。");
        sceneDesc.model = "offline";
        logInfo("offline agent script ready");
    } else if (auto config = agent::loadAgentConfig(configPath_, envFilePath_);
               !config.has_value()) {
        logError("agent config: {}", config.error());
    } else {
        const detail::SessionPlan plan = detail::planSessions(*config, confirmDestructiveOverride_);
        wantConfirm = plan.confirmDestructive;
        if (plan.sceneEnabled) {
            sceneProvider_ =
                makeHttpProvider(plan.sceneEndpoint, config->requestTimeout, sceneRetryNotice_);
            sceneDesc.model = plan.sceneEndpoint.model;
            sceneDesc.systemPrompt = kSceneSystemPrompt;
            sceneDesc.maxTokens = config->maxTokens;
            sceneDesc.reasoningEffort = config->reasoningEffort;
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
            shaderProvider_ =
                makeHttpProvider(plan.shaderEndpoint, config->requestTimeout, shaderRetryNotice_);
            shaderDesc.model = plan.shaderEndpoint.model;
            shaderDesc.systemPrompt = kShaderSystemPrompt;
            shaderDesc.maxTokens = config->maxTokens;
            shaderDesc.reasoningEffort = config->reasoningEffort;
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
        confirmGate_.emplace();
    }
    if (isReload) {
        // Chat transcripts live app-side; the app keeps whatever it already
        // drained, so the new session's own transcript needs its own note that
        // history was just reset, not carried over from the old one.
        sceneDesc.initialNotice = kAgentReloadNotice;
        shaderDesc.initialNotice = kAgentReloadNotice;
    }
    if (sceneProvider_ != nullptr) {
        sceneSession_.emplace(*sceneProvider_, sceneToolRegistry_, mainQueue_,
                              confirmGate_.has_value() ? &*confirmGate_ : nullptr, sceneDesc);
    }
    if (shaderProvider_ != nullptr) {
        shaderSession_.emplace(*shaderProvider_, shaderToolRegistry_, mainQueue_,
                               confirmGate_.has_value() ? &*confirmGate_ : nullptr, shaderDesc);
    }
}

std::unique_ptr<EngineRuntime> EngineRuntime::create(gpu::Device& device, const Desc& desc) {
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
    self->assetDir_ = desc.assetDir;
    self->configPath_ = desc.configPath;
    self->envFilePath_ = desc.envFilePath;
    self->offline_ = desc.offline;
    self->confirmDestructiveOverride_ = desc.confirmDestructive;

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

    // viewer_screenshot (vision feedback loop, M6.97): one definition and one
    // handler shared by the scene assistant's own registry (so it can look at
    // its own work) and the MCP registry below when enabled. Captured by value
    // into each registry, so every copy is just the (cheap) runtime pointer.
    const agent::ToolDef screenshotToolDef{
        .name = "viewer_screenshot",
        .description = "Render the current scene offscreen and save a downscaled PNG; a "
                       "downscaled copy of the image is attached to the result for you to "
                       "inspect.",
        .parametersSchema = R"({"type":"object","properties":{}})",
        .destructive = false};
    auto viewerScreenshot = [runtime = self.get()](std::string_view) -> std::string {
        const gpu::Extent2D extent = runtime->extent_;
        gpu::Ptr<gpu::Texture> target = runtime->device_->createTexture({
            .size = extent,
            .format = kSwapchainFormat,
            .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::CopySrc,
        });
        if (!target) {
            return agent::errorJson("failed to create offscreen render target");
        }
        gpu::Ptr<gpu::CommandEncoder> encoder = runtime->device_->queue().createCommandEncoder();
        runtime->renderer_.render(*encoder, runtime->world_, target.get());
        encoder->finishAndSubmit(nullptr);
        runtime->device_->queue().waitIdle();

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) * extent.height *
                                         4);
        if (!runtime->device_->queue().readTexture(
                *target, pixels.data(), static_cast<std::uint64_t>(extent.width) * 4, extent)) {
            return agent::errorJson("screenshot readback failed");
        }
        // Swapchain is BGRA; PNG wants RGBA.
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
        }
        const asset::DownscaledImage scaled = asset::downscaleRgba(
            pixels.data(), extent.width, extent.height, kScreenshotMaxLongSide);

        // The temp dir is writable from every shell; the SwiftUI app's cwd is
        // whatever LaunchServices provides (often /) and must not be relied on.
        // Per-pid name: the temp dir is shared, and a viewer and the app running
        // side by side must not read each other's (possibly half-written) file.
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                           std::format("kumo_screenshot_{}.png", getpid());
        if (!asset::writePng(path, scaled.width, scaled.height, scaled.rgba.data())) {
            return agent::errorJson(std::format("screenshot write failed: {}", path.string()));
        }
        return nlohmann::json{{"status", "ok"},
                              {"path", path.string()},
                              {"image_path", path.string()},
                              {"width", scaled.width},
                              {"height", scaled.height}}
            .dump();
    };
    self->sceneToolRegistry_.add(screenshotToolDef, viewerScreenshot);

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
    // Lets the shader assistant look at its own edits in situ (docs/agents.md);
    // same tool def and handler as the scene assistant's copy above.
    self->shaderToolRegistry_.add(screenshotToolDef, viewerScreenshot);

    // The MCP registry exposes the full scene tool set (unlike the shader
    // assistant's registry above, which only gets scene_list) plus the shader
    // tools and the same viewer_screenshot tool as the scene assistant (ADR
    // 0041). Tool semantics stay single-sourced: the same contexts and the same
    // screenshot handler back both this registry and the embedded assistants'
    // registries above.
    if (desc.mcp) {
        agent::registerSceneTools(self->mcpToolRegistry_, sceneTools);
        agent::registerShaderTools(self->mcpToolRegistry_, shaderTools);
        self->mcpToolRegistry_.add(screenshotToolDef, viewerScreenshot);
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

    self->assembleAgentSessions(/*isReload=*/false);

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

bool EngineRuntime::reloadAgentSessions() {
    if ((sceneSession_.has_value() && sceneSession_->busy()) ||
        (shaderSession_.has_value() && shaderSession_->busy())) {
        logInfo("agent reload: a session is still running a turn; try again once it finishes");
        return false;
    }
    // Tear down before rebuilding: assembleAgentSessions() assumes these five
    // start empty, same as create() finds them. AgentSession's destructor
    // aborts/joins safely (verified idle above, so this is instant), and no
    // tool call can be mid-confirmation while both sessions are idle.
    sceneSession_.reset();
    shaderSession_.reset();
    sceneProvider_.reset();
    shaderProvider_.reset();
    confirmGate_.reset();
    assembleAgentSessions(/*isReload=*/true);
    logInfo("agent sessions reloaded");
    return true;
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
    // Animated materials (frame.timeParams) need a fresh render every pump even
    // with no other state change, or an on-demand viewport would freeze on the
    // frame the shader was installed; the GLFW viewer renders unconditionally so
    // this is a no-op for it.
    if (renderer_.hasAnimatedMaterials()) {
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

void EngineRuntime::orbitPan(float forward, float right) {
    orbit_.pan(forward, right);
    orbitMoved_ = true;
    markDirty();
}

float EngineRuntime::orbitDistance() const {
    return orbit_.distance();
}

void EngineRuntime::render(gpu::CommandEncoder& encoder, gpu::Texture* output,
                           const renderer::ForwardRenderer::Overlay& overlay) {
    renderer_.render(encoder, world_, output, overlay);
}

void EngineRuntime::resize(gpu::Extent2D size) {
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

bool EngineRuntime::applyEnvironmentSource(const EnvironmentSource& source) {
    asset::HdrImage image;
    if (!source.file.empty()) {
        const auto hdr = asset::loadHdr(assetDir_ / "env" / source.file);
        if (!hdr.has_value()) {
            logError("applyEnvironment: failed to load {}: {}", source.file, hdr.error());
            return false;
        }
        image = *hdr;
    } else {
        image = asset::proceduralSky(source.sky);
    }
    const renderer::ibl::Environment environment = renderer::ibl::bake(*device_, image);
    if (!environment.valid()) {
        logError("applyEnvironment: IBL bake failed");
        return false;
    }
    if (!renderer_.setEnvironment(environment)) {
        logError("applyEnvironment: renderer setEnvironment failed");
        return false;
    }
    environmentSky_ = source;
    return true;
}

bool EngineRuntime::applyEnvironment(const asset::ProceduralSkyDesc& desc) {
    const bool ok = applyEnvironmentSource(EnvironmentSource{.file = "", .sky = desc});
    if (ok) {
        markDirty();
    }
    return ok;
}

bool EngineRuntime::applyEnvironmentFile(const std::string& file) {
    const bool ok = applyEnvironmentSource(EnvironmentSource{.file = file});
    if (ok) {
        markDirty();
    }
    return ok;
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
    const scene::ShaderLookup shaderLookup =
        [this](std::int32_t materialIndex) -> std::optional<std::string> {
        if (materialIndex < 0) {
            return std::nullopt;
        }
        const std::string* source =
            renderer_.materialShaderSource(static_cast<std::uint32_t>(materialIndex));
        return source != nullptr ? std::make_optional(*source) : std::nullopt;
    };
    const std::optional<scene::SavedEnvironment> savedEnvironment =
        environmentSky_.has_value() ? std::make_optional(toSavedEnvironment(*environmentSky_))
                                    : std::nullopt;
    const std::string json =
        scene::saveSceneJson(world_, modelPath_.string(), lookup, savedEnvironment, shaderLookup);
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

    // Absent key leaves the current environment (HDR, procedural or file) untouched.
    if (saved.environment.has_value()) {
        if (!applyEnvironmentSource(toEnvironmentSource(*saved.environment))) {
            logError("scene load: failed to apply the saved environment; keeping the current "
                     "environment");
        }
    }

    std::size_t loaded = 0;
    for (const scene::SavedEntity& savedEntity : saved.entities) {
        scene::Entity entity = savedEntity.entity;
        if (!entity.model.empty()) {
            // loadOrGetModel caches by file, so entities that share a model
            // re-instantiate the upload only on the first one encountered.
            const UploadedModel* model = loadOrGetModel(entity.model);
            if (model == nullptr) {
                logError("scene load: failed to load model '{}' for entity '{}', skipping",
                         entity.model, entity.name);
                continue;
            }
            if (entity.modelMesh < 0 ||
                static_cast<std::size_t>(entity.modelMesh) >= model->meshIndices.size()) {
                logError("scene load: model mesh index {} out of range for entity '{}', skipping",
                         entity.modelMesh, entity.name);
                continue;
            }
            entity.meshIndex = model->meshIndices[static_cast<std::size_t>(entity.modelMesh)];
            entity.materialIndex = model->meshMaterial[static_cast<std::size_t>(entity.modelMesh)];
            if (savedEntity.material.has_value() && entity.materialIndex >= 0) {
                renderer_.setMaterialParams(static_cast<std::uint32_t>(entity.materialIndex),
                                            toMaterialParams(*savedEntity.material));
            }
        } else if (!entity.primitive.empty()) {
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
        // Custom shader (shader-assistant output, ADR 0011): installed after the
        // material's factors are in place so the rebuilt factor buffer picks up
        // the saved values. A compile failure never blocks the load; the entity
        // simply keeps rendering with the standard pbr pipeline.
        if (savedEntity.shaderSource.has_value() && entity.materialIndex >= 0) {
            if (auto result = renderer_.setMaterialShader(
                    static_cast<std::uint32_t>(entity.materialIndex), *savedEntity.shaderSource);
                !result.has_value()) {
                logError("scene load: custom shader for entity '{}' failed to compile, keeping "
                         "the standard pipeline",
                         entity.name);
                for (const shaderc::CompileError& error : result.error()) {
                    logError("{}", error.message);
                }
            }
        }
        world_.entities.insert(entity);
        ++loaded;
    }
    logInfo("scene loaded: {} entities, {} lights", loaded, saved.lights.size());
    markDirty();
    return true;
}

const EngineRuntime::UploadedModel* EngineRuntime::loadOrGetModel(const std::string& name) {
    if (const auto it = modelCache_.find(name); it != modelCache_.end()) {
        return &it->second;
    }
    const std::filesystem::path path = assetDir_ / "models" / (name + ".glb");
    auto sceneAsset = asset::loadGltf(path);
    if (!sceneAsset.has_value()) {
        logError("instantiateModel: failed to load {}: {}", path.string(), sceneAsset.error());
        return nullptr;
    }

    UploadedModel uploaded;
    uploaded.meshIndices.reserve(sceneAsset->meshes.size());
    for (const asset::MeshData& mesh : sceneAsset->meshes) {
        const std::int32_t index = renderer_.addMesh(mesh);
        if (index < 0) {
            logError("instantiateModel: mesh upload failed for {}", path.string());
            return nullptr;
        }
        uploaded.meshIndices.push_back(index);
    }

    std::vector<std::int32_t> textureIndices;
    textureIndices.reserve(sceneAsset->textures.size());
    for (const asset::TextureData& tex : sceneAsset->textures) {
        const std::int32_t index = renderer_.addTexture(tex);
        if (index < 0) {
            logError("instantiateModel: texture upload failed for {}", path.string());
            return nullptr;
        }
        textureIndices.push_back(index);
    }
    const auto remapTexture = [&](std::int32_t local) -> std::int32_t {
        return local >= 0 && static_cast<std::size_t>(local) < textureIndices.size()
                   ? textureIndices[static_cast<std::size_t>(local)]
                   : -1;
    };

    std::vector<std::int32_t> materialIndices;
    materialIndices.reserve(sceneAsset->materials.size());
    for (const asset::MaterialData& mat : sceneAsset->materials) {
        MaterialParams params;
        std::copy(std::begin(mat.baseColor), std::end(mat.baseColor), params.baseColor);
        params.metallic = mat.metallic;
        params.roughness = mat.roughness;
        std::copy(std::begin(mat.emissive), std::end(mat.emissive), params.emissive);
        const renderer::ForwardRenderer::MaterialTextureIndices textures{
            .baseColor = remapTexture(mat.baseColorTexture),
            .metallicRoughness = remapTexture(mat.metallicRoughnessTexture),
            .normal = remapTexture(mat.normalTexture),
            .occlusion = remapTexture(mat.occlusionTexture),
            .emissive = remapTexture(mat.emissiveTexture),
        };
        const std::int32_t index = renderer_.addMaterial(params, textures);
        if (index < 0) {
            logError("instantiateModel: material upload failed for {}", path.string());
            return nullptr;
        }
        materialIndices.push_back(index);
    }

    uploaded.meshMaterial.reserve(sceneAsset->meshes.size());
    for (const asset::MeshData& mesh : sceneAsset->meshes) {
        const std::int32_t local = mesh.materialIndex;
        uploaded.meshMaterial.push_back(local >= 0 && static_cast<std::size_t>(local) <
                                                          materialIndices.size()
                                            ? materialIndices[static_cast<std::size_t>(local)]
                                            : -1);
    }
    uploaded.nodes = sceneAsset->nodes;

    logInfo("instantiateModel: uploaded {}: {} meshes, {} materials, {} textures, {} nodes",
            path.filename().string(), uploaded.meshIndices.size(), materialIndices.size(),
            textureIndices.size(), uploaded.nodes.size());
    return &modelCache_.emplace(name, std::move(uploaded)).first->second;
}

std::expected<EngineRuntime::ModelInstance, std::string>
EngineRuntime::instantiateModel(std::string_view name, const scene::Transform& root) {
    // Non-uniform root scale composed onto a rotated child node cannot be
    // represented as a TRS (it shears); reject it like scene_instance_group
    // does rather than silently producing a wrong transform.
    constexpr float kEps = 1e-5f;
    if (std::abs(root.scale.x - root.scale.y) > kEps ||
        std::abs(root.scale.x - root.scale.z) > kEps) {
        return std::unexpected(std::string("instantiateModel: root scale must be uniform"));
    }

    const std::string modelName(name);
    const UploadedModel* model = loadOrGetModel(modelName);
    if (model == nullptr) {
        return std::unexpected(
            std::format("instantiateModel: failed to load or upload '{}'", modelName));
    }

    ModelInstance instance;
    instance.entities.reserve(model->nodes.size());
    const math::float4x4 rootMatrix = root.matrix();
    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        const asset::NodeInstance& node = model->nodes[i];
        if (node.meshIndex < 0 ||
            static_cast<std::size_t>(node.meshIndex) >= model->meshIndices.size()) {
            continue;
        }
        const math::float4x4 worldTransform = rootMatrix * node.worldTransform;
        const math::Trs trs = math::decomposeTrs(worldTransform);
        scene::Entity entity;
        entity.name =
            std::format("{}_{}", modelName, node.name.empty() ? std::to_string(i) : node.name);
        entity.transform = {trs.translation, trs.rotation, trs.scale};
        entity.meshIndex = model->meshIndices[static_cast<std::size_t>(node.meshIndex)];
        entity.materialIndex = model->meshMaterial[static_cast<std::size_t>(node.meshIndex)];
        entity.model = modelName;
        entity.modelMesh = node.meshIndex;
        instance.entities.push_back(world_.entities.insert(entity));
    }
    markDirty();
    return instance;
}

std::vector<EngineRuntime::EntityInfo> EngineRuntime::listEntities() const {
    std::vector<EntityInfo> out;
    world_.entities.forEach([&](scene::EntityId id, const scene::Entity& entity) {
        out.push_back({.id = agent::formatEntityId(id),
                       .name = entity.name,
                       .primitive = entity.primitive,
                       .model = entity.model});
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
    detail.model = entity->model;
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

EngineRuntime::LightState EngineRuntime::sunLightState() const {
    LightState state;
    const auto lights = world_.lights();
    if (lights.empty()) {
        return state;
    }
    state.found = true;
    // Mirrors apps/viewer/ui.cpp's LightSettings::syncFrom.
    const scene::Light& light = lights[0];
    const math::float3 toSource = -light.direction;
    const float len = math::length(toSource);
    if (len > 1e-4f) {
        const math::float3 dir = toSource / len;
        state.elevationDeg = math::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
        state.azimuthDeg = math::degrees(std::atan2(dir.x, dir.z));
    }
    state.intensity = light.intensity;
    state.color = light.color;
    return state;
}

bool EngineRuntime::setSunLight(float azimuthDeg, float elevationDeg, float intensity,
                                math::float3 color) {
    scene::Light* light = world_.light(0);
    if (light == nullptr || !std::isfinite(azimuthDeg) || !std::isfinite(elevationDeg) ||
        !std::isfinite(intensity) || !isFinite3(color)) {
        return false;
    }
    // Mirrors apps/viewer/ui.cpp's LightSettings::apply.
    const float az = math::radians(azimuthDeg);
    const float el = math::radians(elevationDeg);
    const math::float3 toSource{std::cos(el) * std::sin(az), std::sin(el),
                                std::cos(el) * std::cos(az)};
    light->direction = -toSource;
    light->intensity = intensity;
    light->color = color;
    undo_.commitPending();
    markDirty();
    return true;
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
