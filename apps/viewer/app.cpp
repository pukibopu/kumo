#include "ui.h"

#include <kumo/asset/asset.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/facade/engine_runtime.h>
#include <kumo/gpu/gpu.h>
#include <kumo/gpu/metal_interop.h>
#include <kumo/scene/light.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace CA {
class MetalLayer;
}

CA::MetalLayer* attachMetalLayer(GLFWwindow* window);

namespace {

using namespace kumo;

constexpr gpu::TextureFormat kSwapchainFormat = gpu::TextureFormat::BGRA8Unorm;
constexpr const char* kSceneFileName = "kumo_scene.json";

void configureSurface(gpu::Surface& surface, int width, int height) {
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

struct ScreenshotReadback {
    gpu::Ptr<gpu::Buffer> buffer;
    gpu::Extent2D extent;
};

std::optional<ScreenshotReadback>
encodeScreenshot(gpu::Device& device, gpu::CommandEncoder& encoder, gpu::Texture& texture) {
    const gpu::Extent2D extent = texture.extent();
    const std::uint64_t bytesPerRow = static_cast<std::uint64_t>(extent.width) * 4;
    gpu::Ptr<gpu::Buffer> buffer = device.createBuffer({
        .size = bytesPerRow * extent.height,
        .usage = gpu::BufferUsage::CopyDst | gpu::BufferUsage::CopySrc,
    });
    if (!buffer || !encoder.copyTextureToBuffer(texture, *buffer, 0, bytesPerRow, extent)) {
        return std::nullopt;
    }
    return ScreenshotReadback{.buffer = buffer, .extent = extent};
}

void saveScreenshot(gpu::Device& device, const ScreenshotReadback& readback, int frame) {
    const gpu::Extent2D extent = readback.extent;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    if (!device.queue().readBuffer(*readback.buffer, 0, pixels.data(), pixels.size())) {
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
           const std::filesystem::path& envPath, const std::filesystem::path& assetDir,
           bool demoPrimitives, bool offline, bool confirmDestructive, bool mcp) {
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

    gpu::Ptr<gpu::Device> device = gpu::createDevice();
    if (!device) {
        return fail();
    }
    gpu::Ptr<gpu::Surface> surface =
        gpu::metal::createSurface(*device, attachMetalLayer(window), true);
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
        .assetDir = assetDir,
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
    bool shadowsEnabled = runtime->renderer().shadowsEnabled();
    const bool scriptedSurfaceSmoke = std::getenv("KUMO_VIEWER_SURFACE_SMOKE") != nullptr;

    glfwSetWindowUserPointer(window, &input);
    // Installed before ImGui so its backend chains to this callback.
    glfwSetScrollCallback(window, onScroll);

    bool reloadPending = false;
    kumo::FileWatcher shaderWatcher;
    auto onShaderChange = [&](const std::filesystem::path&) { reloadPending = true; };
    const std::array<const char*, 10> watchedShaders = {
        "pbr.vert",     "pbr.frag",    "skybox.vert", "skybox.frag",         "fullscreen.vert",
        "tonemap.frag", "shadow.vert", "shadow.frag", "include/common.glsl", "include/shadow.glsl"};
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

        if (scriptedSurfaceSmoke) {
            if (frame == 1) {
                glfwSetWindowSize(window, 960, 540);
                logInfo("surface smoke: requested 960x540 resize");
            } else if (frame == 3) {
                glfwIconifyWindow(window);
                logInfo("surface smoke: minimized window");
            } else if (frame == 4) {
                glfwRestoreWindow(window);
                logInfo("surface smoke: restored window");
            }
        }

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
        runtime->renderer().setShadowsEnabled(shadowsEnabled);

        const bool sDown = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        const bool screenshotRequested =
            (scriptedSurfaceSmoke && frame == 2) ||
            (sDown && !input.screenshotKeyDown && !ImGui::GetIO().WantCaptureKeyboard);
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

        std::optional<gpu::SurfaceFrame> surfaceFrame = surface->acquire();
        if (surfaceFrame) {
            gpu::Ptr<gpu::CommandEncoder> encoder = device->queue().createCommandEncoder();
            runtime->render(*encoder, &surfaceFrame->texture(), [&](gpu::RenderPassEncoder& pass) {
                ui::beginFrame(pass);
                ui::drawStatsPanel(fbWidth, fbHeight);
                ui::drawLightPanel(lightSettings, runtime->world().light(0), shadowsEnabled);
                ui::drawMaterialPanel(overrideMetallic, overrideRoughness);
                ui::drawAgentPanels(scenePanel, runtime->sceneSession(),
                                    &runtime->sceneRetryNotice(), shaderPanel,
                                    runtime->shaderSession(), &runtime->shaderRetryNotice());
                ui::drawToolLogPanel(scenePanel, &shaderPanel);
                ui::drawConfirmDialog(runtime->confirmGate());
                ui::endFrame(*encoder, pass);
            });
            std::optional<ScreenshotReadback> screenshot;
            if (screenshotRequested) {
                // The drawable blit belongs before present in this command buffer.
                screenshot = encodeScreenshot(*device, *encoder, surfaceFrame->texture());
            }
            encoder->finishAndSubmit(&*surfaceFrame);
            if (screenshot) {
                saveScreenshot(*device, *screenshot, frame);
            }
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
