#include <kumo/asset/asset.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/renderer/ibl.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>
#include <kumo/scene/scene.h>

#include <Metal/Metal.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_metal.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <vector>

void* attachMetalLayer(GLFWwindow* window);

namespace {

using namespace kumo;

constexpr rhi::TextureFormat kSwapchainFormat = rhi::TextureFormat::BGRA8Unorm;

void configureSurface(rhi::Surface& surface, int width, int height) {
    surface.configure(
        {.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
         .format = kSwapchainFormat});
}

// App-layer camera control (ADR 0039): drag rotates, scroll zooms, and each
// frame the pose is written back to the scene camera via lookAt.
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
        distance = std::clamp(distance * std::exp(-scroll * 0.12f), 0.5f, 20.0f);
    }

    void apply(scene::Camera& camera) const {
        const math::float3 offset{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                  std::cos(pitch) * std::cos(yaw)};
        camera.position = target + offset * distance;
        camera.lookAt(target);
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

void updateOrbit(GLFWwindow* window, AppInput& input) {
    if (input.pendingScroll != 0.0f) {
        input.orbit.zoom(input.pendingScroll);
        input.pendingScroll = 0.0f;
    }
    const bool wantMouse = ImGui::GetIO().WantCaptureMouse;
    const bool pressed =
        !wantMouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (pressed && input.rotating) {
        input.orbit.rotate(static_cast<float>(x - input.lastCursorX),
                           static_cast<float>(y - input.lastCursorY));
    }
    input.rotating = pressed;
    input.lastCursorX = x;
    input.lastCursorY = y;
}

struct LightSettings {
    float azimuthDeg = -45.0f;
    float elevationDeg = 40.0f;
    float intensity = 3.0f;
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};

    void apply(scene::Light& light) const {
        const float az = math::radians(azimuthDeg);
        const float el = math::radians(elevationDeg);
        const math::float3 toSource{std::cos(el) * std::sin(az), std::sin(el),
                                    std::cos(el) * std::cos(az)};
        light.direction = -toSource;
        light.intensity = intensity;
        light.color = {color[0], color[1], color[2]};
    }
};

void imguiInit(rhi::Device& device, GLFWwindow* window) {
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device.nativeHandles().device));
}

void imguiNewFrame(rhi::RenderPassEncoder& pass) {
    ImGui_ImplMetal_NewFrame(
        static_cast<MTL::RenderPassDescriptor*>(pass.nativePassDescriptorHandle()));
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void imguiRender(rhi::CommandEncoder& encoder, rhi::RenderPassEncoder& pass) {
    ImGui::Render();
    ImGui_ImplMetal_RenderDrawData(
        ImGui::GetDrawData(), static_cast<MTL::CommandBuffer*>(encoder.nativeCommandBufferHandle()),
        static_cast<MTL::RenderCommandEncoder*>(pass.nativeEncoderHandle()));
}

void imguiShutdown() {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
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
           const std::filesystem::path& envPath) {
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

    AppInput input;
    LightSettings lightSettings;
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini in the working directory
    ImGui::StyleColorsDark();
    imguiInit(*device, window);

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

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

        updateOrbit(window, input);
        input.orbit.apply(world.camera);
        if (scene::Light* light = world.light(0)) {
            lightSettings.apply(*light);
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
                imguiNewFrame(pass);
                ImGui::SetNextWindowPos({10.0f, 10.0f}, ImGuiCond_FirstUseEver);
                ImGui::Begin("stats");
                ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / ImGui::GetIO().Framerate);
                ImGui::Text("drawable %dx%d", fbWidth, fbHeight);
                ImGui::Text("S: save screenshot");
                ImGui::End();

                ImGui::SetNextWindowPos({10.0f, 110.0f}, ImGuiCond_FirstUseEver);
                ImGui::Begin("light");
                ImGui::SliderFloat("azimuth", &lightSettings.azimuthDeg, -180.0f, 180.0f);
                ImGui::SliderFloat("elevation", &lightSettings.elevationDeg, -85.0f, 85.0f);
                ImGui::SliderFloat("intensity", &lightSettings.intensity, 0.0f, 10.0f);
                ImGui::ColorEdit3("color", lightSettings.color.data());
                ImGui::End();

                ImGui::SetNextWindowPos({10.0f, 270.0f}, ImGuiCond_FirstUseEver);
                ImGui::Begin("material");
                ImGui::SliderFloat("metallic x", &overrideMetallic, 0.0f, 1.0f);
                ImGui::SliderFloat("roughness x", &overrideRoughness, 0.0f, 1.0f);
                ImGui::End();

                imguiRender(*encoder, pass);
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
    imguiShutdown();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
