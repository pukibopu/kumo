#include <kumo/core/file.h>
#include <kumo/core/file_watcher.h>
#include <kumo/core/log.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>
#include <kumo/shaderc/compiler.h>

#include <Metal/Metal.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_metal.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

struct Vertex {
    float x, y;
    float u, v;
};

constexpr std::array<Vertex, 3> kTriangle = {{
    {0.0f, 0.6f, 0.5f, 0.0f},
    {-0.6f, -0.6f, 0.0f, 1.0f},
    {0.6f, -0.6f, 1.0f, 1.0f},
}};

struct ShaderStageInfo {
    const char* sourceName;
    shaderc::Stage stage;
    rhi::ShaderStage rhiStage;
};

constexpr std::array<ShaderStageInfo, 2> kShaderStages = {{
    {"triangle.vert", shaderc::Stage::Vertex, rhi::ShaderStage::Vertex},
    {"triangle.frag", shaderc::Stage::Fragment, rhi::ShaderStage::Fragment},
}};

struct ExpectedBinding {
    std::uint32_t set;
    std::uint32_t binding;
    std::string_view type;
};

// Material contract: fragment reflects set 1 texture + sampler; vertex reflects none.
std::vector<ExpectedBinding> expectedBindings(shaderc::Stage stage) {
    if (stage == shaderc::Stage::Fragment) {
        return {{1, 0, "sampled_texture"}, {1, 1, "sampler"}};
    }
    return {};
}

bool validateReflection(std::string_view sourceName, shaderc::Stage stage,
                        const shaderc::Reflection& reflection) {
    const std::vector<ExpectedBinding> expected = expectedBindings(stage);
    bool ok = true;
    for (const ExpectedBinding& e : expected) {
        const bool found =
            std::any_of(reflection.bindings.begin(), reflection.bindings.end(),
                        [&](const shaderc::ReflectionBinding& b) {
                            return b.set == e.set && b.binding == e.binding && b.type == e.type;
                        });
        if (!found) {
            logError("{}: missing binding set={} binding={} type={}", sourceName, e.set, e.binding,
                     e.type);
            ok = false;
        }
    }
    for (const shaderc::ReflectionBinding& b : reflection.bindings) {
        const bool allowed =
            std::any_of(expected.begin(), expected.end(), [&](const ExpectedBinding& e) {
                return e.set == b.set && e.binding == b.binding && e.type == b.type;
            });
        if (!allowed) {
            logError("{}: unexpected binding set={} binding={} type={}", sourceName, b.set,
                     b.binding, b.type);
            ok = false;
        }
    }
    return ok;
}

rhi::Ptr<rhi::ShaderModule> loadShaderModule(rhi::Device& device, const ShaderStageInfo& info) {
    const std::filesystem::path path = std::filesystem::path(KUMO_SHADER_DIR) / info.sourceName;
    auto source = readTextFile(path);
    if (!source) {
        logError("{}: {}", path.string(), source.error());
        return nullptr;
    }
    auto compiled = shaderc::compileGlsl(*source, info.stage, {.sourceName = info.sourceName});
    if (!compiled) {
        for (const shaderc::CompileError& err : compiled.error()) {
            logError("{}", shaderc::formatError(err, info.sourceName));
        }
        return nullptr;
    }
    if (!validateReflection(info.sourceName, info.stage, compiled->reflection)) {
        return nullptr;
    }
    return device.createShaderModule({
        .stage = info.rhiStage,
        .language = rhi::ShaderSourceLanguage::MSL,
        .source = compiled->msl,
        .entryPoint = compiled->mslEntryPoint,
    });
}

rhi::Ptr<rhi::RenderPipeline> buildPipeline(rhi::Device& device,
                                            const rhi::Ptr<rhi::ShaderModule>& vertexShader,
                                            const rhi::Ptr<rhi::ShaderModule>& fragmentShader,
                                            const rhi::Ptr<rhi::BindGroupLayout>& materialLayout) {
    return device.createRenderPipeline({
        .vertexShader = vertexShader,
        .fragmentShader = fragmentShader,
        .vertexBuffers =
            {{.stride = sizeof(Vertex),
              .attributes =
                  {{.format = rhi::VertexFormat::Float32x2, .offset = 0, .shaderLocation = 0},
                   {.format = rhi::VertexFormat::Float32x2, .offset = 8, .shaderLocation = 1}}}},
        .bindGroupLayouts = {nullptr, materialLayout},
        .colorFormats = {kSwapchainFormat},
    });
}

rhi::Ptr<rhi::Texture> makeCheckerTexture(rhi::Device& device) {
    constexpr std::uint32_t kSize = 8;
    std::array<std::uint32_t, kSize * kSize> pixels{};
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool odd = ((x + y) & 1u) != 0;
            pixels[y * kSize + x] = odd ? 0xff2fa4e0u : 0xfff2f2f2u; // ABGR in memory
        }
    }
    rhi::Ptr<rhi::Texture> texture = device.createTexture({
        .size = {kSize, kSize},
        .format = rhi::TextureFormat::RGBA8Unorm,
        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
    });
    device.queue().writeTexture(*texture, pixels.data(), kSize * 4, {kSize, kSize});
    return texture;
}

} // namespace

int runApp(int maxFrames) {
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

    rhi::Ptr<rhi::Device> device = rhi::metal::createDevice({});
    if (!device) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    rhi::Ptr<rhi::Surface> surface =
        device->createSurface({.nativeLayer = attachMetalLayer(window)});
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    configureSurface(*surface, fbWidth, fbHeight);

    rhi::Ptr<rhi::Buffer> vertexBuffer = device->createBuffer({
        .size = sizeof(kTriangle),
        .usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
    });
    device->queue().writeBuffer(*vertexBuffer, 0, kTriangle.data(), sizeof(kTriangle));

    rhi::Ptr<rhi::Texture> texture = makeCheckerTexture(*device);
    rhi::Ptr<rhi::Sampler> sampler = device->createSampler({
        .magFilter = rhi::FilterMode::Nearest,
        .minFilter = rhi::FilterMode::Nearest,
    });

    // Material set (set 1): binding 0 texture, binding 1 sampler.
    rhi::Ptr<rhi::BindGroupLayout> materialLayout = device->createBindGroupLayout({
        .entries = {{.binding = 0,
                     .visibility = rhi::ShaderStage::Fragment,
                     .type = rhi::BindingType::Texture},
                    {.binding = 1,
                     .visibility = rhi::ShaderStage::Fragment,
                     .type = rhi::BindingType::Sampler}},
    });
    rhi::Ptr<rhi::BindGroup> materialGroup = device->createBindGroup({
        .layout = materialLayout,
        .entries = {{.binding = 0, .texture = texture}, {.binding = 1, .sampler = sampler}},
    });

    rhi::Ptr<rhi::ShaderModule> vertexShader = loadShaderModule(*device, kShaderStages[0]);
    rhi::Ptr<rhi::ShaderModule> fragmentShader = loadShaderModule(*device, kShaderStages[1]);
    if (!vertexShader || !fragmentShader) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    rhi::Ptr<rhi::RenderPipeline> pipeline =
        buildPipeline(*device, vertexShader, fragmentShader, materialLayout);
    if (!pipeline) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    bool reloadPending = false;
    std::filesystem::path reloadPath;
    kumo::FileWatcher shaderWatcher;
    auto onShaderChange = [&](const std::filesystem::path& changed) {
        reloadPending = true;
        reloadPath = changed;
    };
    for (const ShaderStageInfo& info : kShaderStages) {
        shaderWatcher.watch(std::filesystem::path(KUMO_SHADER_DIR) / info.sourceName,
                            onShaderChange);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini in the working directory
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device->nativeHandles().device));

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        shaderWatcher.poll();
        if (reloadPending) {
            reloadPending = false;
            rhi::Ptr<rhi::ShaderModule> newVertex = loadShaderModule(*device, kShaderStages[0]);
            rhi::Ptr<rhi::ShaderModule> newFragment = loadShaderModule(*device, kShaderStages[1]);
            if (newVertex && newFragment) {
                rhi::Ptr<rhi::RenderPipeline> newPipeline =
                    buildPipeline(*device, newVertex, newFragment, materialLayout);
                if (newPipeline) {
                    pipeline = newPipeline;
                    logInfo("{} reloaded", reloadPath.string());
                }
            }
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width != fbWidth || height != fbHeight) {
            fbWidth = width;
            fbHeight = height;
            configureSurface(*surface, fbWidth, fbHeight);
        }

        rhi::Ptr<rhi::CommandEncoder> encoder = device->queue().createCommandEncoder();
        rhi::Texture* target = surface->acquireNextTexture();
        if (target != nullptr) {
            rhi::RenderPassEncoder& pass = encoder->beginRenderPass({
                .colorAttachments = {{.texture = target,
                                      .loadOp = rhi::LoadOp::Clear,
                                      .clearColor = {0.09f, 0.10f, 0.12f, 1.0f}}},
            });
            pass.setPipeline(*pipeline);
            pass.setVertexBuffer(0, *vertexBuffer);
            pass.setBindGroup(1, *materialGroup);
            pass.draw(3);

            ImGui_ImplMetal_NewFrame(
                static_cast<MTL::RenderPassDescriptor*>(pass.nativePassDescriptorHandle()));
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::Begin("stats");
            ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                        1000.0f / ImGui::GetIO().Framerate);
            ImGui::Text("drawable %dx%d", fbWidth, fbHeight);
            ImGui::End();
            ImGui::Render();
            ImGui_ImplMetal_RenderDrawData(
                ImGui::GetDrawData(),
                static_cast<MTL::CommandBuffer*>(encoder->nativeCommandBufferHandle()),
                static_cast<MTL::RenderCommandEncoder*>(pass.nativeEncoderHandle()));

            pass.end();
        }
        encoder->finishAndSubmit(surface.get());

        ++frame;
        if (maxFrames >= 0 && frame >= maxFrames) {
            break;
        }
    }

    device->queue().waitIdle();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
