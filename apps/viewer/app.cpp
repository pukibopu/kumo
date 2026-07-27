#include "ui.h"

#include <kumo/agent/config.h>
#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/fake_provider.h>
#include <kumo/agent/http_provider.h>
#include <kumo/agent/scene_tools.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/asset/asset.h>
#include <kumo/asset/primitives.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/core/main_thread_queue.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/renderer/ibl.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>
#include <kumo/scene/scene.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
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

} // namespace

int runApp(int maxFrames, const std::filesystem::path& modelPath,
           const std::filesystem::path& envPath, bool demoPrimitives, bool offline,
           bool confirmDestructive) {
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
    // session destructor cancels queued tool work and joins its worker while the
    // state the tools touch is still alive.
    kumo::MainThreadQueue mainQueue;
    agent::ToolRegistry toolRegistry;
    agent::registerSceneTools(toolRegistry, {.scene = &world, .renderer = &renderer});
    ui::RetryNotice retryNotice;
    std::optional<agent::ConfirmationGate> confirmGate;
    std::unique_ptr<agent::ILLMProvider> provider;
    std::optional<agent::AgentSession> session;
    agent::AgentSession::Desc sessionDesc;
    bool wantConfirm = confirmDestructive;
    if (offline) {
        provider = std::make_unique<agent::FakeProvider>(makeOfflineScript(world),
                                                         "离线演示脚本已播放完毕。");
        sessionDesc.model = "offline";
        kumo::logInfo("offline agent script ready");
    } else if (auto config = agent::loadAgentConfig("kumo.config.json", ".env");
               !config.has_value()) {
        kumo::logError("agent config: {}", config.error());
    } else if (!config->agentAvailable()) {
        // Rendering stays fully functional; the panel shows a Chinese hint.
        kumo::logInfo("agent disabled: {}", config->unavailableReason());
    } else {
        wantConfirm = wantConfirm || config->confirmDestructive;
        agent::HttpLLMProvider::Options options;
        options.requestTimeout = config->requestTimeout;
        options.onRetry = [&retryNotice](int attempt, int maxRetries) {
            retryNotice.set(std::format("网络波动，重试中 ({}/{})…", attempt, maxRetries));
        };
        const bool openAi = config->providerType == agent::ProviderType::OpenAi;
        if (openAi) {
            provider = std::make_unique<agent::OpenAiProvider>(config->baseUrl, config->apiKey,
                                                               agent::makeUrlSessionTransport(),
                                                               std::move(options));
        } else {
            provider = std::make_unique<agent::ClaudeProvider>(config->baseUrl, config->apiKey,
                                                               agent::makeUrlSessionTransport(),
                                                               std::move(options));
        }
        sessionDesc.model = config->sceneModel;
        sessionDesc.systemPrompt = kSceneSystemPrompt;
        sessionDesc.maxTokens = config->maxTokens;
        // The key never reaches the log (ADR 0012).
        kumo::logInfo("agent provider ready: {} {} at {}", openAi ? "openai" : "anthropic",
                      config->sceneModel, config->baseUrl);
    }
    if (wantConfirm) {
        confirmGate.emplace();
    }
    if (provider != nullptr) {
        session.emplace(*provider, toolRegistry, mainQueue,
                        confirmGate.has_value() ? &*confirmGate : nullptr, sessionDesc);
    }

    AppInput input;
    ui::LightSettings lightSettings;
    ui::ChatPanel chatPanel;
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

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Tool callbacks land here, outside any command encoder lifetime, so
        // scene changes are frame-atomic (ADR 0005).
        mainQueue.drain();

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

        rhi::Ptr<rhi::CommandEncoder> encoder = device->queue().createCommandEncoder();
        rhi::Texture* target = surface->acquireNextTexture();
        if (target != nullptr) {
            renderer.render(*encoder, world, target, [&](rhi::RenderPassEncoder& pass) {
                ui::beginFrame(pass);
                ui::drawStatsPanel(fbWidth, fbHeight);
                ui::drawLightPanel(lightSettings, world.light(0));
                ui::drawMaterialPanel(overrideMetallic, overrideRoughness);
                ui::drawChatPanel(chatPanel, session.has_value() ? &*session : nullptr,
                                  &retryNotice);
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

    device->queue().waitIdle();
    ui::shutdown();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
