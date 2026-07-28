#include "ui.h"

#include <kumo/agent/config.h>
#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/fake_provider.h>
#include <kumo/agent/http_provider.h>
#include <kumo/agent/mcp_server.h>
#include <kumo/agent/scene_tools.h>
#include <kumo/agent/session.h>
#include <kumo/agent/shader_tools.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/asset/asset.h>
#include <kumo/asset/primitives.h>
#include <kumo/core/file.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/core/main_thread_queue.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/renderer/ibl.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>
#include <kumo/scene/persistence.h>
#include <kumo/scene/scene.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

void* attachMetalLayer(GLFWwindow* window);

namespace {

using namespace kumo;

constexpr rhi::TextureFormat kSwapchainFormat = rhi::TextureFormat::BGRA8Unorm;

// English for tool-call stability; the closing instruction keeps replies in the
// user's language (ADR 0028).
constexpr const char* kSceneSystemPrompt =
    "You are the scene assistant inside kumo, a physically based Metal renderer. "
    "You can only affect the scene through the provided tools; never invent tool names or "
    "entity ids. Coordinates are right-handed with Y up and the camera looking down -Z; "
    "distances are in meters, angles in degrees, colors linear. "
    "Call scene_list before spatial reasoning or edits that depend on current state. "
    "Tool errors come back as JSON with status \"error\": read the message, correct the "
    "call and retry. Keep replies short. Always reply in the user's language.";

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
    "asked otherwise. Always reply in the user's language.";

void configureSurface(rhi::Surface& surface, int width, int height) {
    surface.configure(
        {.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
         .format = kSwapchainFormat});
}

// App-layer camera control (ADR 0039): drag rotates, scroll zooms. The pose is
// written to the scene camera only on user input; otherwise the controller syncs
// itself from the camera, so agent camera_set calls are not overwritten.
struct OrbitController {
    float yaw = 0.0f;
    float pitch = 0.15f;
    float distance = 3.0f;
    math::float3 target{0.0f, 0.0f, 0.0f};

    void rotate(float dx, float dy) {
        yaw -= dx * 0.005f;
        pitch = std::clamp(pitch + dy * 0.005f, -1.5f, 1.5f);
    }

    void zoom(float scroll) {
        // The ceiling follows an agent-imported distance so one scroll notch
        // narrows the range smoothly instead of teleporting back to 20.
        distance =
            std::clamp(distance * std::exp(-scroll * 0.12f), 0.5f, std::max(20.0f, distance));
    }

    void apply(scene::Camera& camera) const {
        const math::float3 offset{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                  std::cos(pitch) * std::cos(yaw)};
        camera.position = target + offset * distance;
        camera.lookAt(target);
    }

    // Adopts the camera's aim as the new pivot: the target moves onto the view
    // ray at the previous pivot distance, so the next drag orbits the agent's
    // framing instead of snapping back to the old target. Imported pitch is
    // clamped into the range rotate() allows.
    void syncFrom(const scene::Camera& camera) {
        const float len = length(camera.position - target);
        if (len > 1e-4f) {
            distance = len;
        }
        const math::float3 forward = camera.rotation * math::float3(0.0f, 0.0f, -1.0f);
        target = camera.position + forward * distance;
        const math::float3 dir = -forward;
        pitch = std::clamp(std::asin(std::clamp(dir.y, -1.0f, 1.0f)), -1.5f, 1.5f);
        yaw = std::atan2(dir.x, dir.z);
    }
};

struct AppInput {
    OrbitController orbit;
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
    bool rotating = false;
    float pendingScroll = 0.0f;
    bool screenshotKeyDown = false;
    bool saveKeyDown = false;
    bool loadKeyDown = false;
};

void onScroll(GLFWwindow* window, double, double yOffset) {
    auto* input = static_cast<AppInput*>(glfwGetWindowUserPointer(window));
    if (input != nullptr && !ImGui::GetIO().WantCaptureMouse) {
        input->pendingScroll += static_cast<float>(yOffset);
    }
}

// True when the user rotated or zoomed this frame.
bool updateOrbit(GLFWwindow* window, AppInput& input) {
    bool moved = false;
    if (input.pendingScroll != 0.0f) {
        input.orbit.zoom(input.pendingScroll);
        input.pendingScroll = 0.0f;
        moved = true;
    }
    const bool wantMouse = ImGui::GetIO().WantCaptureMouse;
    const bool pressed =
        !wantMouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (pressed && input.rotating) {
        const float dx = static_cast<float>(x - input.lastCursorX);
        const float dy = static_cast<float>(y - input.lastCursorY);
        if (dx != 0.0f || dy != 0.0f) {
            input.orbit.rotate(dx, dy);
            moved = true;
        }
    }
    input.rotating = pressed;
    input.lastCursorX = x;
    input.lastCursorY = y;
    return moved;
}

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

void saveScreenshot(rhi::Device& device, rhi::Texture& texture, int frame) {
    const rhi::Extent2D extent = texture.extent();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    if (!device.queue().readTexture(texture, pixels.data(),
                                    static_cast<std::uint64_t>(extent.width) * 4, extent)) {
        return;
    }
    // Swapchain is BGRA; PNG wants RGBA.
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);
    }
    const std::filesystem::path path =
        std::filesystem::current_path() / std::format("screenshot_{}.png", frame);
    if (asset::writePng(path, extent.width, extent.height, pixels.data())) {
        logInfo("screenshot saved: {}", path.string());
    } else {
        logError("screenshot write failed: {}", path.string());
    }
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

constexpr const char* kSceneFileName = "kumo_scene.json";

void saveSceneFile(const scene::Scene& world, const std::filesystem::path& modelPath,
                   const renderer::ForwardRenderer& renderer) {
    const scene::MaterialLookup lookup =
        [&renderer](std::int32_t materialIndex) -> std::optional<scene::SavedMaterial> {
        if (materialIndex < 0) {
            return std::nullopt;
        }
        const MaterialParams* params =
            renderer.materialParams(static_cast<std::uint32_t>(materialIndex));
        return params != nullptr ? std::make_optional(toSavedMaterial(*params)) : std::nullopt;
    };
    const std::string json = scene::saveSceneJson(world, modelPath.string(), lookup);
    const std::filesystem::path path = std::filesystem::current_path() / kSceneFileName;
    std::ofstream out(path, std::ios::binary);
    out << json;
    if (!out) {
        logError("scene save failed: {}", path.string());
        return;
    }
    logInfo("scene saved: {}", path.string());
}

// Rebuilds procedural entities exactly like scene_add_entity does; glTF-sourced
// entities keep their saved mesh/material indices, validated against the
// currently loaded scene since the format does not persist mesh/texture data.
void loadSceneFile(scene::Scene& world, renderer::ForwardRenderer& renderer,
                   const std::filesystem::path& modelPath) {
    const std::filesystem::path path = std::filesystem::current_path() / kSceneFileName;
    if (!std::filesystem::exists(path)) {
        // Expected state, not an error: nothing has been saved yet.
        logInfo("no saved scene at {} (press K to save one first)", path.string());
        return;
    }
    const auto text = readTextFile(path);
    if (!text.has_value()) {
        logError("scene load failed: {}", text.error());
        return;
    }
    const auto parsed = scene::parseSceneJson(*text);
    if (!parsed.has_value()) {
        logError("scene load failed: {}", parsed.error());
        return;
    }
    const scene::SavedScene& saved = *parsed;
    if (saved.modelPath != modelPath.string()) {
        logInfo("scene load: saved model '{}' differs from the current '{}'; proceeding anyway",
                saved.modelPath, modelPath.string());
    }

    world.entities.clear();
    world.clearLights();
    for (std::size_t i = 0; i < saved.lights.size(); ++i) {
        if (!world.addLight(saved.lights[i])) {
            logError("scene load: light budget exhausted; dropping {} of {} lights",
                     saved.lights.size() - i, saved.lights.size());
            break;
        }
    }
    world.camera = saved.camera;

    std::size_t loaded = 0;
    for (const scene::SavedEntity& savedEntity : saved.entities) {
        scene::Entity entity = savedEntity.entity;
        if (!entity.primitive.empty()) {
            asset::MeshData mesh;
            const float half = entity.primitiveSize * 0.5f;
            if (entity.primitive == "sphere") {
                mesh = asset::makeSphere(half);
            } else if (entity.primitive == "cube") {
                mesh = asset::makeCube(half);
            } else if (entity.primitive == "plane") {
                mesh = asset::makePlane(half);
            } else {
                logError("scene load: unknown primitive '{}' on entity '{}', skipping",
                         entity.primitive, entity.name);
                continue;
            }
            const MaterialParams material = savedEntity.material.has_value()
                                                ? toMaterialParams(*savedEntity.material)
                                                : MaterialParams{};
            const std::int32_t meshIndex = renderer.addMesh(mesh);
            const std::int32_t materialIndex = renderer.addMaterial(material);
            if (meshIndex < 0 || materialIndex < 0) {
                logError("scene load: gpu upload failed for entity '{}', skipping", entity.name);
                continue;
            }
            entity.meshIndex = meshIndex;
            entity.materialIndex = materialIndex;
        } else {
            if (entity.meshIndex < 0 ||
                static_cast<std::uint32_t>(entity.meshIndex) >= renderer.meshCount()) {
                logError("scene load: mesh index {} out of range for entity '{}', skipping",
                         entity.meshIndex, entity.name);
                continue;
            }
            if (savedEntity.material.has_value() && entity.materialIndex >= 0) {
                renderer.setMaterialParams(static_cast<std::uint32_t>(entity.materialIndex),
                                           toMaterialParams(*savedEntity.material));
            }
        }
        world.entities.insert(entity);
        ++loaded;
    }
    logInfo("scene loaded: {} entities, {} lights", loaded, saved.lights.size());
}

// Builds an HTTP provider (OpenAI or Anthropic codec, by `endpoint.type`) wired
// to `notice` for retry feedback; shared by the scene and shader sessions so
// each gets its own transport and backoff state.
std::unique_ptr<agent::ILLMProvider> makeHttpProvider(const agent::AgentEndpoint& endpoint,
                                                      std::chrono::seconds requestTimeout,
                                                      ui::RetryNotice& notice) {
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

int runApp(int maxFrames, const std::filesystem::path& modelPath,
           const std::filesystem::path& envPath, bool demoPrimitives, bool offline,
           bool confirmDestructive, bool mcp) {
    auto sceneAsset = asset::loadGltf(modelPath);
    if (!sceneAsset) {
        kumo::logError("{}", sceneAsset.error());
        return 1;
    }
    auto hdr = asset::loadHdr(envPath);
    if (!hdr) {
        kumo::logError("{}", hdr.error());
        return 1;
    }
    kumo::logInfo("loaded {}: {} meshes, {} materials, {} textures, {} nodes",
                  modelPath.filename().string(), sceneAsset->meshes.size(),
                  sceneAsset->materials.size(), sceneAsset->textures.size(),
                  sceneAsset->nodes.size());

    if (!glfwInit()) {
        kumo::logError("glfwInit failed");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "kumo", nullptr, nullptr);
    if (!window) {
        kumo::logError("window creation failed");
        glfwTerminate();
        return 1;
    }
    auto fail = [&] {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    };

    rhi::Ptr<rhi::Device> device = rhi::metal::createDevice({});
    if (!device) {
        return fail();
    }
    rhi::Ptr<rhi::Surface> surface =
        device->createSurface({.nativeLayer = attachMetalLayer(window)});
    if (!surface) {
        return fail();
    }
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    configureSurface(*surface, fbWidth, fbHeight);

    renderer::ForwardRenderer renderer;
    if (!renderer.init(*device, kSwapchainFormat)) {
        return fail();
    }
    const renderer::ibl::Environment environment = renderer::ibl::bake(*device, *hdr);
    if (!environment.valid()) {
        return fail();
    }
    if (!renderer.loadScene(*sceneAsset, environment)) {
        return fail();
    }
    renderer.resize({static_cast<std::uint32_t>(fbWidth), static_cast<std::uint32_t>(fbHeight)});

    scene::Scene world;
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
        world.entities.insert(entity);
    }
    world.addLight({.type = scene::LightType::Directional});
    if (demoPrimitives) {
        addDemoPrimitives(renderer, world);
    }

    // Agent stack. Declared after world/renderer and destroyed before them: the
    // session destructors cancel queued tool work and join their workers while
    // the state the tools touch is still alive. Two registries scope each
    // session to its own tools: the shader assistant gets scene_list (to find
    // entities) plus the shader tools, but not the six scene-editing tools.
    kumo::MainThreadQueue mainQueue;
    agent::ToolRegistry sceneToolRegistry;
    agent::ToolRegistry shaderToolRegistry;
    agent::ToolRegistry mcpToolRegistry;
    agent::registerSceneTools(sceneToolRegistry, {.scene = &world, .renderer = &renderer});
    agent::registerSceneListTool(shaderToolRegistry, {.scene = &world, .renderer = &renderer});
    agent::ShaderToolContext shaderTools;
    shaderTools.scene = &world;
    shaderTools.setShader = [&renderer](std::uint32_t index, std::string_view source) {
        return renderer.setMaterialShader(index, source);
    };
    shaderTools.shaderSource = [&renderer](std::uint32_t index) {
        return renderer.materialShaderSource(index);
    };
    shaderTools.templatePath = std::filesystem::path(KUMO_SHADER_DIR) / "pbr.frag";
    shaderTools.generatedDir = std::filesystem::path(KUMO_SHADER_DIR) / "generated";
    // Shared across the chat registry and the MCP registry below, so the
    // 5-attempt shader_write cap is a single per-material counter regardless
    // of which caller is driving it.
    shaderTools.failureCounts = std::make_shared<std::unordered_map<std::int32_t, int>>();
    agent::registerShaderTools(shaderToolRegistry, shaderTools);

    // The MCP registry exposes the full scene tool set (unlike the shader
    // assistant's registry above, which only gets scene_list) plus the shader
    // tools and a screenshot tool for visual verification (ADR 0041). Tool
    // semantics stay single-sourced: the same contexts back both this
    // registry and the embedded assistants' registries above.
    if (mcp) {
        agent::registerSceneTools(mcpToolRegistry, {.scene = &world, .renderer = &renderer});
        agent::registerShaderTools(mcpToolRegistry, shaderTools);
        mcpToolRegistry.add(
            {.name = "viewer_screenshot",
             .description = "Render the current scene offscreen and save it as a PNG; the "
                            "image is attached to the result.",
             .parametersSchema = R"({"type":"object","properties":{}})",
             .destructive = false},
            [&device, &renderer, &world, &fbWidth, &fbHeight](std::string_view) -> std::string {
                const rhi::Extent2D extent{static_cast<std::uint32_t>(fbWidth),
                                           static_cast<std::uint32_t>(fbHeight)};
                rhi::Ptr<rhi::Texture> target = device->createTexture({
                    .size = extent,
                    .format = kSwapchainFormat,
                    .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                });
                if (!target) {
                    return agent::errorJson("failed to create offscreen render target");
                }
                rhi::Ptr<rhi::CommandEncoder> encoder = device->queue().createCommandEncoder();
                renderer.render(*encoder, world, target.get());
                encoder->finishAndSubmit(nullptr);
                device->queue().waitIdle();

                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) *
                                                 extent.height * 4);
                if (!device->queue().readTexture(*target, pixels.data(),
                                                 static_cast<std::uint64_t>(extent.width) * 4,
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

    ui::RetryNotice sceneRetryNotice;
    ui::RetryNotice shaderRetryNotice;
    std::optional<agent::ConfirmationGate> confirmGate;
    std::unique_ptr<agent::ILLMProvider> sceneProvider;
    std::unique_ptr<agent::ILLMProvider> shaderProvider;
    std::optional<agent::AgentSession> sceneSession;
    std::optional<agent::AgentSession> shaderSession;
    agent::AgentSession::Desc sceneDesc;
    agent::AgentSession::Desc shaderDesc;
    bool wantConfirm = confirmDestructive;
    if (offline) {
        sceneProvider = std::make_unique<agent::FakeProvider>(makeOfflineScript(world),
                                                              "离线演示脚本已播放完毕。");
        sceneDesc.model = "offline";
        kumo::logInfo("offline agent script ready");
    } else if (auto config = agent::loadAgentConfig("kumo.config.json", ".env");
               !config.has_value()) {
        kumo::logError("agent config: {}", config.error());
    } else {
        wantConfirm = wantConfirm || config->confirmDestructive;
        if (config->scene.available()) {
            sceneProvider =
                makeHttpProvider(config->scene, config->requestTimeout, sceneRetryNotice);
            sceneDesc.model = config->scene.model;
            sceneDesc.systemPrompt = kSceneSystemPrompt;
            sceneDesc.maxTokens = config->maxTokens;
            sceneDesc.summaryThresholdTokens = config->summaryThresholdTokens;
            // The key never reaches the log (ADR 0012).
            kumo::logInfo("agent provider ready: {} {} at {}",
                          config->scene.type == agent::ProviderType::OpenAi ? "openai"
                                                                            : "anthropic",
                          config->scene.model, config->scene.baseUrl);
        } else {
            // Rendering stays fully functional; the panel shows a Chinese hint.
            kumo::logInfo("agent disabled: {}", config->scene.unavailableReason());
        }
        if (config->shader.available()) {
            shaderProvider =
                makeHttpProvider(config->shader, config->requestTimeout, shaderRetryNotice);
            shaderDesc.model = config->shader.model;
            shaderDesc.systemPrompt = kShaderSystemPrompt;
            shaderDesc.maxTokens = config->maxTokens;
            shaderDesc.summaryThresholdTokens = config->summaryThresholdTokens;
            kumo::logInfo("shader agent ready: {} {} at {}",
                          config->shader.type == agent::ProviderType::OpenAi ? "openai"
                                                                             : "anthropic",
                          config->shader.model, config->shader.baseUrl);
        } else {
            kumo::logInfo("shader agent disabled: {}", config->shader.unavailableReason());
        }
    }
    if (wantConfirm) {
        confirmGate.emplace();
    }
    if (sceneProvider != nullptr) {
        sceneSession.emplace(*sceneProvider, sceneToolRegistry, mainQueue,
                             confirmGate.has_value() ? &*confirmGate : nullptr, sceneDesc);
    }
    if (shaderProvider != nullptr) {
        shaderSession.emplace(*shaderProvider, shaderToolRegistry, mainQueue,
                              confirmGate.has_value() ? &*confirmGate : nullptr, shaderDesc);
    }

    AppInput input;
    ui::LightSettings lightSettings;
    ui::ChatPanel scenePanel;
    ui::ChatPanel shaderPanel;
    float overrideMetallic = 1.0f;
    float overrideRoughness = 1.0f;

    glfwSetWindowUserPointer(window, &input);
    // Installed before ImGui so its backend chains to this callback.
    glfwSetScrollCallback(window, onScroll);

    bool reloadPending = false;
    kumo::FileWatcher shaderWatcher;
    auto onShaderChange = [&](const std::filesystem::path&) { reloadPending = true; };
    const std::array<const char*, 7> watchedShaders = {
        "pbr.vert",        "pbr.frag",     "skybox.vert",        "skybox.frag",
        "fullscreen.vert", "tonemap.frag", "include/common.glsl"};
    for (const char* name : watchedShaders) {
        shaderWatcher.watch(std::filesystem::path(KUMO_SHADER_DIR) / name, onShaderChange);
    }

    ui::init(*device, window);
    input.orbit.apply(world.camera);
    if (scene::Light* light = world.light(0)) {
        // The slider defaults define the startup look; from here on the panel
        // applies its own edits and syncFrom mirrors everything else.
        lightSettings.apply(*light);
    }

    // MCP stdio pump (ADR 0041): one JSON-RPC line in, at most one line out.
    // handleMessage runs on the main thread by being posted through mainQueue,
    // same as every other tool callback, so it never races the render loop.
    agent::McpServer mcpServer(mcpToolRegistry, "kumo", KUMO_VERSION_STRING);
    std::atomic<bool> mcpEof{false};
    std::thread mcpReader;
    if (mcp) {
        mcpReader = std::thread([&] {
            std::string line;
            while (std::getline(std::cin, line)) {
                const std::string response =
                    mainQueue
                        .post([&mcpServer, line] {
                            return mcpServer.handleMessage(line).value_or(std::string());
                        })
                        .get();
                if (!response.empty()) {
                    std::cout << response << '\n' << std::flush;
                }
            }
            mcpEof.store(true);
        });
    }

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Tool callbacks land here, outside any command encoder lifetime, so
        // scene changes are frame-atomic (ADR 0005).
        mainQueue.drain();
        if (mcpEof.load()) {
            // The MCP client hung up (stdin closed); shut down cleanly.
            break;
        }

        shaderWatcher.poll();
        if (reloadPending) {
            reloadPending = false;
            if (renderer.reloadPipelines()) {
                kumo::logInfo("shader pipelines reloaded");
            }
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width != fbWidth || height != fbHeight) {
            fbWidth = width;
            fbHeight = height;
            configureSurface(*surface, fbWidth, fbHeight);
            renderer.resize(
                {static_cast<std::uint32_t>(fbWidth), static_cast<std::uint32_t>(fbHeight)});
        }

        if (updateOrbit(window, input)) {
            input.orbit.apply(world.camera);
        } else {
            input.orbit.syncFrom(world.camera);
        }
        // Sliders mirror the light; the panel applies user edits itself in the
        // same frame, so agent light_set changes are never overwritten here.
        if (scene::Light* light = world.light(0)) {
            lightSettings.syncFrom(*light);
        }
        renderer.setMaterialOverride(overrideMetallic, overrideRoughness);

        const bool sDown = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        const bool screenshotRequested =
            sDown && !input.screenshotKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.screenshotKeyDown = sDown;

        const bool kDown = glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS;
        const bool saveRequested =
            kDown && !input.saveKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.saveKeyDown = kDown;
        if (saveRequested) {
            saveSceneFile(world, modelPath, renderer);
        }

        const bool lDown = glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS;
        const bool loadRequested =
            lDown && !input.loadKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.loadKeyDown = lDown;
        if (loadRequested) {
            loadSceneFile(world, renderer, modelPath);
        }

        rhi::Ptr<rhi::CommandEncoder> encoder = device->queue().createCommandEncoder();
        rhi::Texture* target = surface->acquireNextTexture();
        if (target != nullptr) {
            renderer.render(*encoder, world, target, [&](rhi::RenderPassEncoder& pass) {
                ui::beginFrame(pass);
                ui::drawStatsPanel(fbWidth, fbHeight);
                ui::drawLightPanel(lightSettings, world.light(0));
                ui::drawMaterialPanel(overrideMetallic, overrideRoughness);
                ui::drawAgentPanels(scenePanel, sceneSession.has_value() ? &*sceneSession : nullptr,
                                    &sceneRetryNotice, shaderPanel,
                                    shaderSession.has_value() ? &*shaderSession : nullptr,
                                    &shaderRetryNotice);
                ui::drawToolLogPanel(scenePanel, &shaderPanel);
                ui::drawConfirmDialog(confirmGate.has_value() ? &*confirmGate : nullptr);
                ui::endFrame(*encoder, pass);
            });
        }
        encoder->finishAndSubmit(surface.get());
        if (screenshotRequested && target != nullptr) {
            // The frame was just submitted; command buffers execute in commit
            // order, so the readback blit sees the completed, presented image.
            saveScreenshot(*device, *target, frame);
        }

        ++frame;
        if (maxFrames >= 0 && frame >= maxFrames) {
            break;
        }
    }

    if (mcpReader.joinable()) {
        // Unblocks the reader thread's std::getline; must happen before
        // mainQueue (and everything the reader might post to) is torn down.
        ::close(0);
        mcpReader.join();
    }

    device->queue().waitIdle();
    ui::shutdown();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
