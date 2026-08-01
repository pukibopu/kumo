#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <kumo/gpu/gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

using namespace kumo;

namespace {

template <typename T>
inline constexpr bool kConcreteGpuType =
    std::is_final_v<T> && !std::is_abstract_v<T> && !std::is_polymorphic_v<T>;

static_assert(kConcreteGpuType<gpu::Buffer>);
static_assert(kConcreteGpuType<gpu::Texture>);
static_assert(kConcreteGpuType<gpu::Sampler>);
static_assert(kConcreteGpuType<gpu::ShaderModule>);
static_assert(kConcreteGpuType<gpu::BindGroupLayout>);
static_assert(kConcreteGpuType<gpu::BindGroup>);
static_assert(kConcreteGpuType<gpu::RenderPipeline>);
static_assert(kConcreteGpuType<gpu::ComputePipeline>);
static_assert(kConcreteGpuType<gpu::RenderPassEncoder>);
static_assert(kConcreteGpuType<gpu::ComputePassEncoder>);
static_assert(kConcreteGpuType<gpu::Surface>);
static_assert(kConcreteGpuType<gpu::SurfaceFrame>);
static_assert(kConcreteGpuType<gpu::CommandEncoder>);
static_assert(kConcreteGpuType<gpu::Queue>);
static_assert(kConcreteGpuType<gpu::Device>);
static_assert(!std::is_copy_constructible_v<gpu::SurfaceFrame>);
static_assert(std::is_nothrow_move_constructible_v<gpu::SurfaceFrame>);

constexpr const char* kVertexMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut vertex_main(uint vertexId [[vertex_id]]) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    VertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    return out;
}
)";

constexpr const char* kFragmentMsl = R"(
#include <metal_stdlib>
using namespace metal;

fragment float4 fragment_main(texture2d<float> colorTexture [[texture(0)]],
                              sampler colorSampler [[sampler(1)]]) {
    return colorTexture.sample(colorSampler, float2(0.5));
}
)";

constexpr const char* kComputeMsl = R"(
#include <metal_stdlib>
using namespace metal;

kernel void compute_main(device uint* values [[buffer(0)]]) {
    values[0] = 0x1234abcd;
}
)";

gpu::Ptr<gpu::Device> requireDevice() {
    gpu::Ptr<gpu::Device> device = gpu::createDevice();
    REQUIRE(device != nullptr);
    return device;
}

} // namespace

TEST_CASE("gpu facade uploads and reads back buffers and texture views") {
    gpu::Ptr<gpu::Device> device = requireDevice();

    gpu::Ptr<gpu::Buffer> buffer =
        device->createBuffer({.size = 16,
                              .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst |
                                       gpu::BufferUsage::CopySrc});
    REQUIRE(buffer != nullptr);
    const std::array<std::uint32_t, 4> bufferInput = {1, 2, 3, 4};
    std::array<std::uint32_t, 4> bufferOutput{};
    device->queue().writeBuffer(*buffer, 0, bufferInput.data(), sizeof(bufferInput));
    REQUIRE(device->queue().readBuffer(*buffer, 0, bufferOutput.data(), sizeof(bufferOutput)));
    CHECK(bufferOutput == bufferInput);

    const std::array<std::uint8_t, 16> pixels = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    gpu::Ptr<gpu::Texture> texture = device->createTexture({
        .size = {2, 2},
        .format = gpu::TextureFormat::RGBA8Unorm,
        .usage =
            gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst,
    });
    REQUIRE(texture != nullptr);
    device->queue().writeTexture(*texture, pixels.data(), 8, {2, 2});

    gpu::Ptr<gpu::Texture> view = device->createTextureView(texture, {});
    REQUIRE(view != nullptr);
    std::weak_ptr<gpu::Texture> parent = texture;
    texture.reset();
    CHECK(!parent.expired());

    std::array<std::uint8_t, 16> actual{};
    REQUIRE(device->queue().readTexture(*view, actual.data(), 8, {2, 2}));
    CHECK(actual == pixels);
    view.reset();
    CHECK(parent.expired());
}

TEST_CASE("gpu facade renders with bind-group-owned resources and submits") {
    gpu::Ptr<gpu::Device> device = requireDevice();
    gpu::Ptr<gpu::ShaderModule> vertex = device->createShaderModule(
        {.stage = gpu::ShaderStage::Vertex, .mslSource = kVertexMsl, .entryPoint = "vertex_main"});
    gpu::Ptr<gpu::ShaderModule> fragment = device->createShaderModule({
        .stage = gpu::ShaderStage::Fragment,
        .mslSource = kFragmentMsl,
        .entryPoint = "fragment_main",
    });
    REQUIRE(vertex != nullptr);
    REQUIRE(fragment != nullptr);

    gpu::Ptr<gpu::BindGroupLayout> layout = device->createBindGroupLayout({
        .entries = {{.binding = 0,
                     .visibility = gpu::ShaderStage::Fragment,
                     .type = gpu::BindingType::Texture},
                    {.binding = 1,
                     .visibility = gpu::ShaderStage::Fragment,
                     .type = gpu::BindingType::Sampler}},
    });
    gpu::Ptr<gpu::Texture> source = device->createTexture({
        .size = {1, 1},
        .format = gpu::TextureFormat::RGBA8Unorm,
        .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst,
    });
    gpu::Ptr<gpu::Sampler> sampler = device->createSampler({
        .addressModeU = gpu::AddressMode::ClampToEdge,
        .addressModeV = gpu::AddressMode::ClampToEdge,
    });
    REQUIRE(layout != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(sampler != nullptr);
    const std::array<std::uint8_t, 4> red = {255, 0, 0, 255};
    device->queue().writeTexture(*source, red.data(), 4, {1, 1});

    gpu::Ptr<gpu::BindGroup> group = device->createBindGroup({
        .layout = layout,
        .entries = {{.binding = 0, .texture = source}, {.binding = 1, .sampler = sampler}},
    });
    REQUIRE(group != nullptr);
    std::weak_ptr<gpu::Texture> sourceLifetime = source;
    std::weak_ptr<gpu::Sampler> samplerLifetime = sampler;
    source.reset();
    sampler.reset();
    CHECK(!sourceLifetime.expired());
    CHECK(!samplerLifetime.expired());

    gpu::Ptr<gpu::RenderPipeline> pipeline = device->createRenderPipeline({
        .vertexShader = vertex,
        .fragmentShader = fragment,
        .bindGroupLayouts = {layout},
        .colorFormats = {gpu::TextureFormat::RGBA8Unorm},
    });
    REQUIRE(pipeline != nullptr);
    gpu::Ptr<gpu::Texture> target = device->createTexture({
        .size = {2, 2},
        .format = gpu::TextureFormat::RGBA8Unorm,
        .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::CopySrc,
    });
    REQUIRE(target != nullptr);

    gpu::Ptr<gpu::CommandEncoder> commands = device->queue().createCommandEncoder();
    REQUIRE(commands != nullptr);
    gpu::RenderPassEncoder& pass = commands->beginRenderPass({
        .colorAttachments = {{.texture = target.get(),
                              .loadOp = gpu::LoadOp::Clear,
                              .storeOp = gpu::StoreOp::Store}},
    });
    pass.setPipeline(*pipeline);
    pass.setBindGroup(0, *group);
    pass.draw(3);
    pass.end();
    gpu::Ptr<gpu::Buffer> readback = device->createBuffer({
        .size = 16,
        .usage = gpu::BufferUsage::CopyDst | gpu::BufferUsage::CopySrc,
    });
    REQUIRE(readback != nullptr);
    REQUIRE(commands->copyTextureToBuffer(*target, *readback, 0, 8, {2, 2}));
    commands->finishAndSubmit();

    std::array<std::uint8_t, 16> actual{};
    REQUIRE(device->queue().readBuffer(*readback, 0, actual.data(), actual.size()));
    for (std::size_t i = 0; i < actual.size(); i += 4) {
        CHECK(actual[i] == 255);
        CHECK(actual[i + 1] == 0);
        CHECK(actual[i + 2] == 0);
        CHECK(actual[i + 3] == 255);
    }
    device->queue().waitIdle();
}

TEST_CASE("gpu facade creates and dispatches a compute pipeline") {
    gpu::Ptr<gpu::Device> device = requireDevice();
    gpu::Ptr<gpu::ShaderModule> shader = device->createShaderModule({
        .stage = gpu::ShaderStage::Compute,
        .mslSource = kComputeMsl,
        .entryPoint = "compute_main",
    });
    gpu::Ptr<gpu::BindGroupLayout> layout = device->createBindGroupLayout({
        .entries = {{.binding = 0,
                     .visibility = gpu::ShaderStage::Compute,
                     .type = gpu::BindingType::StorageBuffer}},
    });
    gpu::Ptr<gpu::Buffer> buffer =
        device->createBuffer({.size = sizeof(std::uint32_t),
                              .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::CopySrc});
    REQUIRE(shader != nullptr);
    REQUIRE(layout != nullptr);
    REQUIRE(buffer != nullptr);
    const std::uint32_t zero = 0;
    device->queue().writeBuffer(*buffer, 0, &zero, sizeof(zero));

    gpu::Ptr<gpu::BindGroup> group = device->createBindGroup({
        .layout = layout,
        .entries = {{.binding = 0, .buffer = buffer}},
    });
    gpu::Ptr<gpu::ComputePipeline> pipeline = device->createComputePipeline({
        .shader = shader,
        .bindGroupLayouts = {layout},
        .workgroupSizeX = 1,
    });
    REQUIRE(group != nullptr);
    REQUIRE(pipeline != nullptr);
    std::weak_ptr<gpu::Buffer> bufferLifetime = buffer;
    buffer.reset();
    CHECK(!bufferLifetime.expired());

    gpu::Ptr<gpu::CommandEncoder> commands = device->queue().createCommandEncoder();
    REQUIRE(commands != nullptr);
    gpu::ComputePassEncoder& pass = commands->beginComputePass();
    pass.setPipeline(*pipeline);
    pass.setBindGroup(0, *group);
    pass.dispatch(1);
    pass.end();
    commands->finishAndSubmit();

    gpu::Ptr<gpu::Buffer> retained = bufferLifetime.lock();
    REQUIRE(retained != nullptr);
    std::uint32_t result = 0;
    REQUIRE(device->queue().readBuffer(*retained, 0, &result, sizeof(result)));
    CHECK(result == 0x1234abcd);
    device->queue().waitIdle();
}

TEST_CASE("gpu facade rejects invalid descriptors") {
    gpu::Ptr<gpu::Device> device = requireDevice();

    CHECK(device->createBuffer({}) == nullptr);
    CHECK(device->createTexture({}) == nullptr);
    CHECK(device->createTexture(
              {.size = {1, 1}, .format = gpu::TextureFormat::RGBA8Unorm, .sampleCount = 2}) ==
          nullptr);
    CHECK(device->createSampler({.maxAnisotropy = 0}) == nullptr);
    CHECK(device->createShaderModule({}) == nullptr);
    CHECK(device->createBindGroup({}) == nullptr);
    CHECK(device->createRenderPipeline({}) == nullptr);
    CHECK(device->createComputePipeline({}) == nullptr);
    CHECK(device->createBindGroupLayout({
              .entries = {{.binding = 0, .visibility = gpu::ShaderStage::Fragment},
                          {.binding = 0, .visibility = gpu::ShaderStage::Fragment}},
          }) == nullptr);

    gpu::Ptr<gpu::BindGroupLayout> layout = device->createBindGroupLayout({
        .entries = {{.binding = 0,
                     .visibility = gpu::ShaderStage::Compute,
                     .type = gpu::BindingType::StorageBuffer}},
    });
    REQUIRE(layout != nullptr);
    CHECK(device->createBindGroup({.layout = layout}) == nullptr);

    gpu::Ptr<gpu::Buffer> buffer = device->createBuffer({.size = 4});
    REQUIRE(buffer != nullptr);
    gpu::Ptr<gpu::BindGroupLayout> twoEntryLayout = device->createBindGroupLayout({
        .entries = {{.binding = 0,
                     .visibility = gpu::ShaderStage::Compute,
                     .type = gpu::BindingType::StorageBuffer},
                    {.binding = 1,
                     .visibility = gpu::ShaderStage::Compute,
                     .type = gpu::BindingType::StorageBuffer}},
    });
    REQUIRE(twoEntryLayout != nullptr);
    CHECK(device->createBindGroup({
              .layout = twoEntryLayout,
              .entries = {{.binding = 0, .buffer = buffer}, {.binding = 0, .buffer = buffer}},
          }) == nullptr);
    CHECK(device->createBindGroup({
              .layout = layout,
              .entries = {{.binding = 0,
                           .texture = device->createTexture({
                               .size = {1, 1},
                               .format = gpu::TextureFormat::RGBA8Unorm,
                               .usage = gpu::TextureUsage::Sampled,
                           })}},
          }) == nullptr);

    gpu::Ptr<gpu::BindGroupLayout> overflowingSamplerLayout = device->createBindGroupLayout({
        .entries = {{.binding = 6,
                     .visibility = gpu::ShaderStage::Compute,
                     .type = gpu::BindingType::Sampler}},
    });
    gpu::Ptr<gpu::ShaderModule> compute = device->createShaderModule({
        .stage = gpu::ShaderStage::Compute,
        .mslSource = kComputeMsl,
        .entryPoint = "compute_main",
    });
    REQUIRE(overflowingSamplerLayout != nullptr);
    REQUIRE(compute != nullptr);
    CHECK(device->createComputePipeline({
              .shader = compute,
              .bindGroupLayouts = {nullptr, nullptr, overflowingSamplerLayout},
          }) == nullptr);
}
