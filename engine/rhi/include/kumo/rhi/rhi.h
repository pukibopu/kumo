#pragma once

#include "kumo/rhi/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kumo::rhi {

template <typename T> using Ptr = std::shared_ptr<T>;

#define KUMO_RHI_RESOURCE(T)                                                                       \
public:                                                                                            \
    virtual ~T() = default;                                                                        \
    T(const T&) = delete;                                                                          \
    T& operator=(const T&) = delete;                                                               \
    T(T&&) = delete;                                                                               \
    T& operator=(T&&) = delete;                                                                    \
                                                                                                   \
protected:                                                                                         \
    T() = default;

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
};

class Buffer {
    KUMO_RHI_RESOURCE(Buffer)
public:
    virtual std::uint64_t size() const = 0;
};

struct TextureDesc {
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureUsage usage = TextureUsage::None;
    TextureDimension dimension = TextureDimension::Tex2D;
    std::uint32_t mipLevelCount = 1;
    std::uint32_t sampleCount = 1;
};

class Texture {
    KUMO_RHI_RESOURCE(Texture)
public:
    virtual Extent2D extent() const = 0;
    virtual TextureFormat format() const = 0;
};

struct SamplerDesc {
    FilterMode magFilter = FilterMode::Linear;
    FilterMode minFilter = FilterMode::Linear;
    FilterMode mipFilter = FilterMode::Linear;
    AddressMode addressModeU = AddressMode::Repeat;
    AddressMode addressModeV = AddressMode::Repeat;
    AddressMode addressModeW = AddressMode::Repeat;
};

class Sampler {
    KUMO_RHI_RESOURCE(Sampler)
};

struct ShaderModuleDesc {
    ShaderStage stage = ShaderStage::None;
    ShaderSourceLanguage language = ShaderSourceLanguage::MSL;
    std::string source;               // used when language == MSL
    std::vector<std::uint32_t> spirv; // used when language == SPIRV
    std::string entryPoint;
};

class ShaderModule {
    KUMO_RHI_RESOURCE(ShaderModule)
};

struct BindGroupLayoutEntry {
    std::uint32_t binding = 0;
    ShaderStage visibility = ShaderStage::None;
    BindingType type = BindingType::UniformBuffer;
};

struct BindGroupLayoutDesc {
    std::vector<BindGroupLayoutEntry> entries;
};

class BindGroupLayout {
    KUMO_RHI_RESOURCE(BindGroupLayout)
};

class Sampler;
class Texture;

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

class BindGroup {
    KUMO_RHI_RESOURCE(BindGroup)
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

class RenderPipeline {
    KUMO_RHI_RESOURCE(RenderPipeline)
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
    // Reversed-Z clear value.
    float clearDepth = 0.0f;
};

struct RenderPassDesc {
    std::vector<RenderPassColorAttachment> colorAttachments;
    RenderPassDepthAttachment depthAttachment;
};

class RenderPassEncoder {
    KUMO_RHI_RESOURCE(RenderPassEncoder)
public:
    virtual void setPipeline(RenderPipeline& pipeline) = 0;
    virtual void setBindGroup(std::uint32_t set, BindGroup& group) = 0;
    virtual void setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset = 0) = 0;
    virtual void setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset = 0) = 0;
    virtual void setPushConstants(ShaderStage stages, const void* data, std::uint32_t size) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1,
                      std::uint32_t firstVertex = 0) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1,
                             std::uint32_t firstIndex = 0) = 0;
    virtual void end() = 0;
    // Escape hatch for ImGui backends; MTLRenderCommandEncoder* on Metal.
    virtual void* nativeEncoderHandle() = 0;
    virtual void* nativePassDescriptorHandle() = 0;
};

struct SurfaceDesc {
    // CAMetalLayer* on Apple platforms.
    void* nativeLayer = nullptr;
    // VkSurfaceKHR on Vulkan.
    void* nativeSurface = nullptr;
};

struct SurfaceConfig {
    Extent2D size;
    TextureFormat format = TextureFormat::BGRA8Unorm;
};

class Surface {
    KUMO_RHI_RESOURCE(Surface)
public:
    virtual void configure(const SurfaceConfig& config) = 0;
    // Transient texture, valid until the frame is submitted. Null when no
    // drawable is available (e.g. window minimized).
    virtual Texture* acquireNextTexture() = 0;
    // Backbuffer count; sizes the ImGui backend. Metal keeps 3 drawables.
    virtual std::uint32_t imageCount() const { return 3; }
};

class CommandEncoder {
    KUMO_RHI_RESOURCE(CommandEncoder)
public:
    // The returned encoder is owned by this command encoder and valid until end().
    virtual RenderPassEncoder& beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void finishAndSubmit(Surface* presentTo = nullptr) = 0;
    // MTLCommandBuffer* on Metal.
    virtual void* nativeCommandBufferHandle() = 0;
};

class Queue {
    KUMO_RHI_RESOURCE(Queue)
public:
    // One command encoder per frame; creation throttles to the frames in flight.
    virtual Ptr<CommandEncoder> createCommandEncoder() = 0;
    // Blocks until all submitted work completes; call before tearing down surfaces.
    virtual void waitIdle() = 0;
    virtual void writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                             std::uint64_t size) = 0;
    virtual void writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                              Extent2D size) = 0;
};

struct DeviceDesc {
    bool enableValidation = true;
};

struct NativeHandles {
    // MTLDevice* on Metal.
    void* device = nullptr;
    // Vulkan handles for the ImGui backend; null/0 on Metal.
    void* vkInstance = nullptr;
    void* vkPhysicalDevice = nullptr;
    void* vkDevice = nullptr;
    void* vkQueue = nullptr;
    std::uint32_t vkQueueFamily = 0;
};

class Device {
    KUMO_RHI_RESOURCE(Device)
public:
    virtual Ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
    virtual Ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    virtual Ptr<Sampler> createSampler(const SamplerDesc& desc) = 0;
    virtual Ptr<ShaderModule> createShaderModule(const ShaderModuleDesc& desc) = 0;
    virtual Ptr<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
    virtual Ptr<BindGroup> createBindGroup(const BindGroupDesc& desc) = 0;
    virtual Ptr<RenderPipeline> createRenderPipeline(const RenderPipelineDesc& desc) = 0;
    virtual Ptr<Surface> createSurface(const SurfaceDesc& desc) = 0;
    virtual Queue& queue() = 0;
    virtual NativeHandles nativeHandles() = 0;
};

#undef KUMO_RHI_RESOURCE

} // namespace kumo::rhi
