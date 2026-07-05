#include <kumo/core/log.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>

#include <Metal/Metal.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_metal.h>
#include <imgui.h>

#include <array>
#include <cstdint>

void* attachMetalLayer(GLFWwindow* window);

namespace {

using namespace kumo;

constexpr const char* kTriangleMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        texture2d<float> baseColor [[texture(8)]],
                        sampler baseSampler [[sampler(9)]]) {
    return baseColor.sample(baseSampler, in.uv);
}
)";

struct Vertex {
    float x, y;
    float u, v;
};

constexpr std::array<Vertex, 3> kTriangle = {{
    {0.0f, 0.6f, 0.5f, 0.0f},
    {-0.6f, -0.6f, 0.0f, 1.0f},
    {0.6f, -0.6f, 1.0f, 1.0f},
}};

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
    surface->configure({.size = {static_cast<std::uint32_t>(fbWidth),
                                 static_cast<std::uint32_t>(fbHeight)},
                        .format = rhi::TextureFormat::BGRA8Unorm});

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

    rhi::Ptr<rhi::ShaderModule> vertexShader = device->createShaderModule({
        .stage = rhi::ShaderStage::Vertex,
        .source = kTriangleMSL,
        .entryPoint = "vs_main",
    });
    rhi::Ptr<rhi::ShaderModule> fragmentShader = device->createShaderModule({
        .stage = rhi::ShaderStage::Fragment,
        .source = kTriangleMSL,
        .entryPoint = "fs_main",
    });

    rhi::Ptr<rhi::RenderPipeline> pipeline = device->createRenderPipeline({
        .vertexShader = vertexShader,
        .fragmentShader = fragmentShader,
        .vertexBuffers = {{.stride = sizeof(Vertex),
                           .attributes = {{.format = rhi::VertexFormat::Float32x2,
                                           .offset = 0,
                                           .shaderLocation = 0},
                                          {.format = rhi::VertexFormat::Float32x2,
                                           .offset = 8,
                                           .shaderLocation = 1}}}},
        .bindGroupLayouts = {nullptr, materialLayout},
        .colorFormats = {rhi::TextureFormat::BGRA8Unorm},
    });
    if (!pipeline) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device->nativeHandles().device));

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width != fbWidth || height != fbHeight) {
            fbWidth = width;
            fbHeight = height;
            surface->configure({.size = {static_cast<std::uint32_t>(fbWidth),
                                         static_cast<std::uint32_t>(fbHeight)},
                                .format = rhi::TextureFormat::BGRA8Unorm});
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

    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
