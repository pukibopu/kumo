#pragma once

#include "kumo/gpu/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kumo::gpu {

template <typename T> using Ptr = std::shared_ptr<T>;

namespace detail {
struct MetalInterop;
}

class Buffer;
class Texture;
class Sampler;
class ShaderModule;
class BindGroupLayout;
class BindGroup;
class RenderPipeline;
class ComputePipeline;
class RenderPassEncoder;
class ComputePassEncoder;
class Surface;
class SurfaceFrame;
class CommandEncoder;
class Queue;
class Device;

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
};

class Buffer final {
public:
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) = delete;
    Buffer& operator=(Buffer&&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept;

private:
    struct Impl;
    explicit Buffer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class Queue;
    friend class CommandEncoder;
    friend class RenderPassEncoder;
    friend class ComputePassEncoder;
    friend struct detail::MetalInterop;
};

struct TextureDesc {
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureUsage usage = TextureUsage::None;
    TextureDimension dimension = TextureDimension::Tex2D;
    std::uint32_t mipLevelCount = 1;
    std::uint32_t sampleCount = 1;
    StorageMode storageMode = StorageMode::Automatic;
};

class Texture final {
public:
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] Extent2D extent() const noexcept;
    [[nodiscard]] TextureFormat format() const noexcept;
    [[nodiscard]] std::uint32_t sampleCount() const noexcept;

private:
    struct Impl;
    explicit Texture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class Queue;
    friend class CommandEncoder;
    friend class RenderPassEncoder;
    friend class ComputePassEncoder;
    friend class Surface;
    friend class SurfaceFrame;
    friend struct detail::MetalInterop;
};

// A view is a Texture over a subrange of a parent texture. The view retains the
// parent and can be used anywhere a regular Texture is accepted.
struct TextureViewDesc {
    std::uint32_t baseMipLevel = 0;
    std::uint32_t mipLevelCount = 1;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t arrayLayerCount = 1;
    TextureDimension dimension = TextureDimension::Tex2D;
};

struct SamplerDesc {
    FilterMode magFilter = FilterMode::Linear;
    FilterMode minFilter = FilterMode::Linear;
    FilterMode mipFilter = FilterMode::Linear;
    AddressMode addressModeU = AddressMode::Repeat;
    AddressMode addressModeV = AddressMode::Repeat;
    AddressMode addressModeW = AddressMode::Repeat;
    float lodMaxClamp = 1000.0f;
    std::uint32_t maxAnisotropy = 1;
    CompareFunction compare = CompareFunction::Never;
};

class Sampler final {
public:
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Sampler(Sampler&&) = delete;
    Sampler& operator=(Sampler&&) = delete;

private:
    struct Impl;
    explicit Sampler(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class ComputePassEncoder;
    friend class RenderPassEncoder;
    friend struct detail::MetalInterop;
};

struct ShaderModuleDesc {
    ShaderStage stage = ShaderStage::None;
    std::string mslSource;
    std::string entryPoint;
};

class ShaderModule final {
public:
    ~ShaderModule();
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;
    ShaderModule(ShaderModule&&) = delete;
    ShaderModule& operator=(ShaderModule&&) = delete;

private:
    struct Impl;
    explicit ShaderModule(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend struct detail::MetalInterop;
};

struct BindGroupLayoutEntry {
    std::uint32_t binding = 0;
    ShaderStage visibility = ShaderStage::None;
    BindingType type = BindingType::UniformBuffer;
};

struct BindGroupLayoutDesc {
    std::vector<BindGroupLayoutEntry> entries;
};

class BindGroupLayout final {
public:
    ~BindGroupLayout();
    BindGroupLayout(const BindGroupLayout&) = delete;
    BindGroupLayout& operator=(const BindGroupLayout&) = delete;
    BindGroupLayout(BindGroupLayout&&) = delete;
    BindGroupLayout& operator=(BindGroupLayout&&) = delete;

private:
    struct Impl;
    explicit BindGroupLayout(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend struct detail::MetalInterop;
};

struct BindGroupEntry {
    std::uint32_t binding = 0;
    Ptr<Buffer> buffer;
    std::uint64_t bufferOffset = 0;
    Ptr<Texture> texture;
    Ptr<Sampler> sampler;
};

struct BindGroupDesc {
    Ptr<BindGroupLayout> layout;
    std::vector<BindGroupEntry> entries;
};

class BindGroup final {
public:
    ~BindGroup();
    BindGroup(const BindGroup&) = delete;
    BindGroup& operator=(const BindGroup&) = delete;
    BindGroup(BindGroup&&) = delete;
    BindGroup& operator=(BindGroup&&) = delete;

private:
    struct Impl;
    explicit BindGroup(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class RenderPassEncoder;
    friend class ComputePassEncoder;
    friend struct detail::MetalInterop;
};

struct VertexAttribute {
    VertexFormat format = VertexFormat::Float32;
    std::uint32_t offset = 0;
    std::uint32_t shaderLocation = 0;
};

struct VertexBufferLayout {
    std::uint32_t stride = 0;
    VertexStepMode stepMode = VertexStepMode::Vertex;
    std::vector<VertexAttribute> attributes;
};

struct DepthStencilState {
    TextureFormat format = TextureFormat::Undefined;
    bool depthWriteEnabled = false;
    // Reversed-Z: near is 1, far is 0.
    CompareFunction depthCompare = CompareFunction::GreaterEqual;
    float depthBias = 0.0f;
    float depthBiasSlopeScale = 0.0f;
    float depthBiasClamp = 0.0f;
};

struct RenderPipelineDesc {
    Ptr<ShaderModule> vertexShader;
    Ptr<ShaderModule> fragmentShader;
    std::vector<VertexBufferLayout> vertexBuffers;
    std::vector<Ptr<BindGroupLayout>> bindGroupLayouts;
    std::uint32_t pushConstantSize = 0;
    std::vector<TextureFormat> colorFormats;
    DepthStencilState depthStencil;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cullMode = CullMode::None;
    FrontFace frontFace = FrontFace::CCW;
    std::uint32_t sampleCount = 1;
};

class RenderPipeline final {
public:
    ~RenderPipeline();
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;

private:
    struct Impl;
    explicit RenderPipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class RenderPassEncoder;
    friend struct detail::MetalInterop;
};

struct RenderPassColorAttachment {
    Texture* texture = nullptr;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    Color clearColor;
    Texture* resolveTarget = nullptr;
};

struct RenderPassDepthAttachment {
    Texture* texture = nullptr;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::DontCare;
    float clearDepth = 0.0f;
};

struct RenderPassDesc {
    std::vector<RenderPassColorAttachment> colorAttachments;
    RenderPassDepthAttachment depthAttachment;
};

class RenderPassEncoder final {
public:
    ~RenderPassEncoder();
    RenderPassEncoder(const RenderPassEncoder&) = delete;
    RenderPassEncoder& operator=(const RenderPassEncoder&) = delete;
    RenderPassEncoder(RenderPassEncoder&&) = delete;
    RenderPassEncoder& operator=(RenderPassEncoder&&) = delete;

    void setPipeline(RenderPipeline& pipeline);
    void setBindGroup(std::uint32_t set, BindGroup& group);
    void setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset = 0);
    void setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset = 0);
    void setPushConstants(ShaderStage stages, const void* data, std::uint32_t size);
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1,
              std::uint32_t firstVertex = 0);
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1,
                     std::uint32_t firstIndex = 0);
    void end();

private:
    struct Impl;
    RenderPassEncoder();
    std::unique_ptr<Impl> impl_;

    friend class CommandEncoder;
    friend struct detail::MetalInterop;
};

struct ComputePipelineDesc {
    Ptr<ShaderModule> shader;
    std::vector<Ptr<BindGroupLayout>> bindGroupLayouts;
    std::uint32_t pushConstantSize = 0;
    std::uint32_t workgroupSizeX = 1;
    std::uint32_t workgroupSizeY = 1;
    std::uint32_t workgroupSizeZ = 1;
};

class ComputePipeline final {
public:
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&) = delete;
    ComputePipeline& operator=(ComputePipeline&&) = delete;

private:
    struct Impl;
    explicit ComputePipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend class ComputePassEncoder;
    friend struct detail::MetalInterop;
};

class ComputePassEncoder final {
public:
    ~ComputePassEncoder();
    ComputePassEncoder(const ComputePassEncoder&) = delete;
    ComputePassEncoder& operator=(const ComputePassEncoder&) = delete;
    ComputePassEncoder(ComputePassEncoder&&) = delete;
    ComputePassEncoder& operator=(ComputePassEncoder&&) = delete;

    void setPipeline(ComputePipeline& pipeline);
    void setBindGroup(std::uint32_t set, BindGroup& group);
    void setPushConstants(const void* data, std::uint32_t size);
    void dispatch(std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1);
    void end();

private:
    struct Impl;
    ComputePassEncoder();
    std::unique_ptr<Impl> impl_;

    friend class CommandEncoder;
    friend struct detail::MetalInterop;
};

struct SurfaceConfig {
    Extent2D size;
    TextureFormat format = TextureFormat::BGRA8Unorm;
};

class SurfaceFrame final {
public:
    ~SurfaceFrame();
    SurfaceFrame(const SurfaceFrame&) = delete;
    SurfaceFrame& operator=(const SurfaceFrame&) = delete;
    SurfaceFrame(SurfaceFrame&&) noexcept;
    SurfaceFrame& operator=(SurfaceFrame&&) noexcept;

    [[nodiscard]] Texture& texture() noexcept;
    [[nodiscard]] const Texture& texture() const noexcept;

private:
    struct Impl;
    explicit SurfaceFrame(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Surface;
    friend class CommandEncoder;
    friend struct detail::MetalInterop;
};

class Surface final {
public:
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) = delete;
    Surface& operator=(Surface&&) = delete;

    void configure(const SurfaceConfig& config);
    [[nodiscard]] std::optional<SurfaceFrame> acquire();
    [[nodiscard]] TextureFormat format() const noexcept;

private:
    struct Impl;
    explicit Surface(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend struct detail::MetalInterop;
};

class CommandEncoder final {
public:
    ~CommandEncoder();
    CommandEncoder(const CommandEncoder&) = delete;
    CommandEncoder& operator=(const CommandEncoder&) = delete;
    CommandEncoder(CommandEncoder&&) = delete;
    CommandEncoder& operator=(CommandEncoder&&) = delete;

    RenderPassEncoder& beginRenderPass(const RenderPassDesc& desc);
    ComputePassEncoder& beginComputePass();
    void generateMipmaps(Texture& texture);
    [[nodiscard]] bool copyTextureToBuffer(Texture& texture, Buffer& destination,
                                           std::uint64_t destinationOffset,
                                           std::uint64_t bytesPerRow, Extent2D size);
    void finishAndSubmit(SurfaceFrame* presentFrame = nullptr);

private:
    struct Impl;
    explicit CommandEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Queue;
    friend struct detail::MetalInterop;
};

class Queue final {
public:
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    [[nodiscard]] Ptr<CommandEncoder> createCommandEncoder();
    void waitIdle();
    void writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size);
    [[nodiscard]] bool readBuffer(Buffer& buffer, std::uint64_t offset, void* out,
                                  std::uint64_t size);
    void writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow, Extent2D size);
    [[nodiscard]] bool readTexture(Texture& texture, void* out, std::uint64_t bytesPerRow,
                                   Extent2D size);

private:
    struct Impl;
    explicit Queue(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Device;
    friend struct detail::MetalInterop;
};

class Device final {
public:
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] Ptr<Buffer> createBuffer(const BufferDesc& desc);
    [[nodiscard]] Ptr<Texture> createTexture(const TextureDesc& desc);
    [[nodiscard]] Ptr<Texture> createTextureView(const Ptr<Texture>& texture,
                                                 const TextureViewDesc& desc);
    [[nodiscard]] Ptr<Sampler> createSampler(const SamplerDesc& desc);
    [[nodiscard]] Ptr<ShaderModule> createShaderModule(const ShaderModuleDesc& desc);
    [[nodiscard]] Ptr<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDesc& desc);
    [[nodiscard]] Ptr<BindGroup> createBindGroup(const BindGroupDesc& desc);
    [[nodiscard]] Ptr<RenderPipeline> createRenderPipeline(const RenderPipelineDesc& desc);
    [[nodiscard]] Ptr<ComputePipeline> createComputePipeline(const ComputePipelineDesc& desc);
    [[nodiscard]] Queue& queue() noexcept;

private:
    struct Impl;
    explicit Device(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend Ptr<Device> createDevice();
    friend struct detail::MetalInterop;
};

[[nodiscard]] Ptr<Device> createDevice();

} // namespace kumo::gpu
