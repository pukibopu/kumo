#include "ui.h"

#include <kumo/asset/asset.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/facade/engine_runtime.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>
#include <kumo/scene/light.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

void* attachMetalLayer(GLFWwindow* window);

namespace {

using namespace kumo;

constexpr rhi::TextureFormat kSwapchainFormat = rhi::TextureFormat::BGRA8Unorm;
constexpr const char* kSceneFileName = "kumo_scene.json";

void configureSurface(rhi::Surface& surface, int width, int height) {
    surface.configure(
        {.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
         .format = kSwapchainFormat});
}

struct AppInput {
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

// Cursor tracking, WantCaptureMouse gating and scroll accumulation stay in the
// shell; rotate/zoom deltas forward to the runtime's orbit camera (ADR 0039),
// which arbitrates against agent camera_set calls once per frame in pump().
void updateOrbit(GLFWwindow* window, AppInput& input, facade::EngineRuntime& runtime) {
    if (input.pendingScroll != 0.0f) {
        runtime.orbitZoom(input.pendingScroll);
        input.pendingScroll = 0.0f;
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
            runtime.orbitRotate(dx, dy);
        }
    }
    input.rotating = pressed;
    input.lastCursorX = x;
    input.lastCursorY = y;
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
           bool confirmDestructive, bool mcp) {
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

    facade::EngineRuntime::Desc runtimeDesc{
        .modelPath = modelPath,
        .envPath = envPath,
        .configPath = "kumo.config.json",
        .envFilePath = ".env",
        .shaderDir = KUMO_SHADER_DIR,
        .offline = offline,
        .confirmDestructive = confirmDestructive,
        .demoPrimitives = demoPrimitives,
        .mcp = mcp,
        .appVersion = KUMO_VERSION_STRING,
    };
    std::unique_ptr<facade::EngineRuntime> runtime =
        facade::EngineRuntime::create(*device, runtimeDesc);
    if (!runtime) {
        return fail();
    }
    runtime->resize({static_cast<std::uint32_t>(fbWidth), static_cast<std::uint32_t>(fbHeight)});

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
    if (scene::Light* light = runtime->world().light(0)) {
        // The slider defaults define the startup look; from here on the panel
        // applies its own edits and syncFrom mirrors everything else.
        lightSettings.apply(*light);
    }

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Input deltas must reach the runtime before pump() runs the orbit
        // arbitration, or the rendered camera lags the drag by one frame.
        updateOrbit(window, input, *runtime);

        if (!runtime->pump()) {
            // The MCP client hung up (stdin closed); shut down cleanly.
            break;
        }

        shaderWatcher.poll();
        if (reloadPending) {
            reloadPending = false;
            if (runtime->reloadPipelines()) {
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
            runtime->resize(
                {static_cast<std::uint32_t>(fbWidth), static_cast<std::uint32_t>(fbHeight)});
        }

        // Sliders mirror the light; the panel applies user edits itself in the
        // same frame, so agent light_set changes are never overwritten here.
        if (scene::Light* light = runtime->world().light(0)) {
            lightSettings.syncFrom(*light);
        }
        runtime->renderer().setMaterialOverride(overrideMetallic, overrideRoughness);

        const bool sDown = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        const bool screenshotRequested =
            sDown && !input.screenshotKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.screenshotKeyDown = sDown;

        const bool kDown = glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS;
        const bool saveRequested =
            kDown && !input.saveKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.saveKeyDown = kDown;
        if (saveRequested) {
            runtime->saveScene(std::filesystem::current_path() / kSceneFileName);
        }

        const bool lDown = glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS;
        const bool loadRequested =
            lDown && !input.loadKeyDown && !ImGui::GetIO().WantCaptureKeyboard;
        input.loadKeyDown = lDown;
        if (loadRequested) {
            runtime->loadScene(std::filesystem::current_path() / kSceneFileName);
        }

        rhi::Ptr<rhi::CommandEncoder> encoder = device->queue().createCommandEncoder();
        rhi::Texture* target = surface->acquireNextTexture();
        if (target != nullptr) {
            runtime->render(*encoder, target, [&](rhi::RenderPassEncoder& pass) {
                ui::beginFrame(pass);
                ui::drawStatsPanel(fbWidth, fbHeight);
                ui::drawLightPanel(lightSettings, runtime->world().light(0));
                ui::drawMaterialPanel(overrideMetallic, overrideRoughness);
                ui::drawAgentPanels(scenePanel, runtime->sceneSession(),
                                    &runtime->sceneRetryNotice(), shaderPanel,
                                    runtime->shaderSession(), &runtime->shaderRetryNotice());
                ui::drawToolLogPanel(scenePanel, &shaderPanel);
                ui::drawConfirmDialog(runtime->confirmGate());
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

    // Drops the runtime first: its destructor stops/joins the MCP reader and
    // waits for the GPU to go idle before any window/ImGui teardown below.
    runtime.reset();
    ui::shutdown();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
