#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "kumo/gpu/gpu.h"
#include "kumo/gpu/metal_interop.h"

#include "kumo/core/assert.h"
#include "kumo/core/log.h"
#include "kumo/shaderabi/metal_binding.h"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace kumo::gpu {

namespace binding = ::kumo::shaderabi::metal;

template <typename T>
inline constexpr bool kConcreteGpuType =
    std::is_final_v<T> && !std::is_abstract_v<T> && !std::is_polymorphic_v<T>;

static_assert(kConcreteGpuType<Buffer>);
static_assert(kConcreteGpuType<Texture>);
static_assert(kConcreteGpuType<Sampler>);
static_assert(kConcreteGpuType<ShaderModule>);
static_assert(kConcreteGpuType<BindGroupLayout>);
static_assert(kConcreteGpuType<BindGroup>);
static_assert(kConcreteGpuType<RenderPipeline>);
static_assert(kConcreteGpuType<ComputePipeline>);
static_assert(kConcreteGpuType<RenderPassEncoder>);
static_assert(kConcreteGpuType<ComputePassEncoder>);
static_assert(kConcreteGpuType<SurfaceFrame>);
static_assert(kConcreteGpuType<Surface>);
static_assert(kConcreteGpuType<CommandEncoder>);
static_assert(kConcreteGpuType<Queue>);
static_assert(kConcreteGpuType<Device>);

namespace {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxPushConstantSize = 128;

MTL::PixelFormat toMtl(TextureFormat format) {
    switch (format) {
    case TextureFormat::Undefined:
        return MTL::PixelFormatInvalid;
    case TextureFormat::RGBA8Unorm:
        return MTL::PixelFormatRGBA8Unorm;
    case TextureFormat::RGBA8UnormSrgb:
        return MTL::PixelFormatRGBA8Unorm_sRGB;
    case TextureFormat::BGRA8Unorm:
        return MTL::PixelFormatBGRA8Unorm;
    case TextureFormat::BGRA8UnormSrgb:
        return MTL::PixelFormatBGRA8Unorm_sRGB;
    case TextureFormat::RGBA16Float:
        return MTL::PixelFormatRGBA16Float;
    case TextureFormat::RG16Float:
        return MTL::PixelFormatRG16Float;
    case TextureFormat::R32Float:
        return MTL::PixelFormatR32Float;
    case TextureFormat::RGBA32Float:
        return MTL::PixelFormatRGBA32Float;
    case TextureFormat::Depth32Float:
        return MTL::PixelFormatDepth32Float;
    }
    return MTL::PixelFormatInvalid;
}

MTL::VertexFormat toMtl(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float32:
        return MTL::VertexFormatFloat;
    case VertexFormat::Float32x2:
        return MTL::VertexFormatFloat2;
    case VertexFormat::Float32x3:
        return MTL::VertexFormatFloat3;
    case VertexFormat::Float32x4:
        return MTL::VertexFormatFloat4;
    case VertexFormat::Uint32:
        return MTL::VertexFormatUInt;
    case VertexFormat::Unorm8x4:
        return MTL::VertexFormatUChar4Normalized;
    }
    return MTL::VertexFormatInvalid;
}

MTL::CompareFunction toMtl(CompareFunction function) {
    switch (function) {
    case CompareFunction::Never:
        return MTL::CompareFunctionNever;
    case CompareFunction::Less:
        return MTL::CompareFunctionLess;
    case CompareFunction::Equal:
        return MTL::CompareFunctionEqual;
    case CompareFunction::LessEqual:
        return MTL::CompareFunctionLessEqual;
    case CompareFunction::Greater:
        return MTL::CompareFunctionGreater;
    case CompareFunction::NotEqual:
        return MTL::CompareFunctionNotEqual;
    case CompareFunction::GreaterEqual:
        return MTL::CompareFunctionGreaterEqual;
    case CompareFunction::Always:
        return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionAlways;
}

MTL::SamplerMinMagFilter toMtl(FilterMode mode) {
    return mode == FilterMode::Nearest ? MTL::SamplerMinMagFilterNearest
                                       : MTL::SamplerMinMagFilterLinear;
}

MTL::SamplerAddressMode toMtl(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat:
        return MTL::SamplerAddressModeRepeat;
    case AddressMode::MirrorRepeat:
        return MTL::SamplerAddressModeMirrorRepeat;
    case AddressMode::ClampToEdge:
        return MTL::SamplerAddressModeClampToEdge;
    }
    return MTL::SamplerAddressModeRepeat;
}

MTL::LoadAction toMtl(LoadOp op) {
    switch (op) {
    case LoadOp::Load:
        return MTL::LoadActionLoad;
    case LoadOp::Clear:
        return MTL::LoadActionClear;
    case LoadOp::DontCare:
        return MTL::LoadActionDontCare;
    }
    return MTL::LoadActionClear;
}

MTL::StoreAction toMtl(StoreOp op) {
    return op == StoreOp::Store ? MTL::StoreActionStore : MTL::StoreActionDontCare;
}

MTL::PrimitiveType toMtlPrimitive(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return MTL::PrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip:
        return MTL::PrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList:
        return MTL::PrimitiveTypeLine;
    }
    return MTL::PrimitiveTypeTriangle;
}

NS::String* nsString(const std::string& text) {
    return NS::String::string(text.c_str(), NS::UTF8StringEncoding);
}

bool isFilterable(TextureFormat format) {
    return format != TextureFormat::RGBA32Float;
}

std::uint32_t bytesPerPixel(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::RGBA8UnormSrgb:
    case TextureFormat::BGRA8Unorm:
    case TextureFormat::BGRA8UnormSrgb:
    case TextureFormat::RG16Float:
    case TextureFormat::R32Float:
    case TextureFormat::Depth32Float:
        return 4;
    case TextureFormat::RGBA16Float:
        return 8;
    case TextureFormat::RGBA32Float:
        return 16;
    case TextureFormat::Undefined:
        return 0;
    }
    return 0;
}

bool isSingleShaderStage(ShaderStage stage) {
    return stage == ShaderStage::Vertex || stage == ShaderStage::Fragment ||
           stage == ShaderStage::Compute;
}

bool isTransientDescriptorValid(const TextureDesc& desc) {
    const TextureUsage forbidden = TextureUsage::Sampled | TextureUsage::Storage |
                                   TextureUsage::CopySrc | TextureUsage::CopyDst;
    return hasFlag(desc.usage, TextureUsage::RenderTarget) && !hasFlag(desc.usage, forbidden) &&
           desc.dimension == TextureDimension::Tex2D && desc.mipLevelCount == 1;
}

bool supportsMemoryless(MTL::Device* device) {
    if (__builtin_available(macOS 11.0, iOS 10.0, tvOS 10.0, *)) {
        return device->supportsFamily(MTL::GPUFamilyApple1);
    }
    return false;
}

bool checkedDataSize(std::uint64_t bytesPerRow, Extent2D size, std::uint64_t& result) {
    if (size.height != 0 && bytesPerRow > std::numeric_limits<std::uint64_t>::max() / size.height) {
        return false;
    }
    result = bytesPerRow * size.height;
    return true;
}

bool layoutFitsMetalSet(const BindGroupLayoutDesc& layout, std::uint32_t set) {
    return std::ranges::all_of(layout.entries, [set](const BindGroupLayoutEntry& entry) {
        return entry.type != BindingType::Sampler ||
               binding::samplerIndex(set, entry.binding) <= binding::kMaxSamplerIndex;
    });
}

struct AutoreleasePoolGuard {
    AutoreleasePoolGuard() : pool(NS::AutoreleasePool::alloc()->init()) {}
    ~AutoreleasePoolGuard() { pool->release(); }
    AutoreleasePoolGuard(const AutoreleasePoolGuard&) = delete;
    AutoreleasePoolGuard& operator=(const AutoreleasePoolGuard&) = delete;
    NS::AutoreleasePool* pool;
};

} // namespace

struct Buffer::Impl {
    NS::SharedPtr<MTL::Buffer> buffer;
    std::uint64_t size = 0;
};

struct Texture::Impl {
    NS::SharedPtr<MTL::Texture> texture;
    Ptr<Texture> parent;
    Extent2D extent;
    TextureFormat format = TextureFormat::Undefined;
    TextureDimension dimension = TextureDimension::Tex2D;
    std::uint32_t mipLevelCount = 1;
    std::uint32_t arrayLayerCount = 1;
    std::uint32_t sampleCount = 1;
    bool drawable = false;
};

struct Sampler::Impl {
    NS::SharedPtr<MTL::SamplerState> sampler;
};

struct ShaderModule::Impl {
    NS::SharedPtr<MTL::Library> library;
    NS::SharedPtr<MTL::Function> function;
    ShaderStage stage = ShaderStage::None;
};

struct BindGroupLayout::Impl {
    BindGroupLayoutDesc desc;
};

struct BindGroup::Impl {
    struct ResolvedEntry {
        BindGroupEntry entry;
        ShaderStage visibility = ShaderStage::None;
        BindingType type = BindingType::UniformBuffer;
    };

    std::vector<ResolvedEntry> resolved;
};

struct RenderPipeline::Impl {
    NS::SharedPtr<MTL::RenderPipelineState> state;
    NS::SharedPtr<MTL::DepthStencilState> depthStencil;
    MTL::PrimitiveType primitive = MTL::PrimitiveTypeTriangle;
    MTL::CullMode cullMode = MTL::CullModeNone;
    MTL::Winding winding = MTL::WindingCounterClockwise;
    std::uint32_t sampleCount = 1;
    float depthBias = 0.0f;
    float depthBiasSlopeScale = 0.0f;
    float depthBiasClamp = 0.0f;
};

struct ComputePipeline::Impl {
    NS::SharedPtr<MTL::ComputePipelineState> state;
    MTL::Size threadsPerGroup = MTL::Size::Make(1, 1, 1);
};

struct RenderPassEncoder::Impl {
    NS::SharedPtr<MTL::RenderCommandEncoder> encoder;
    NS::SharedPtr<MTL::RenderPassDescriptor> passDescriptor;
    MTL::PrimitiveType primitive = MTL::PrimitiveTypeTriangle;
    MTL::Buffer* indexBuffer = nullptr;
    MTL::IndexType indexType = MTL::IndexTypeUInt16;
    std::uint64_t indexOffset = 0;
    std::uint32_t passSampleCount = 1;
    bool open = false;

    void reset() {
        encoder.reset();
        passDescriptor.reset();
        indexBuffer = nullptr;
        indexOffset = 0;
        passSampleCount = 1;
        open = false;
    }
};

struct ComputePassEncoder::Impl {
    NS::SharedPtr<MTL::ComputeCommandEncoder> encoder;
    MTL::Size threadsPerGroup = MTL::Size::Make(1, 1, 1);
    bool open = false;

    void reset() {
        encoder.reset();
        threadsPerGroup = MTL::Size::Make(1, 1, 1);
        open = false;
    }
};

struct SurfaceFrame::Impl {
    NS::SharedPtr<CA::MetalDrawable> drawable;
    std::unique_ptr<Texture> texture;
    bool presented = false;
};

struct Surface::Impl {
    NS::SharedPtr<CA::MetalLayer> layer;
    Extent2D extent;
    TextureFormat format = TextureFormat::BGRA8Unorm;
};

struct CommandEncoder::Impl {
    NS::AutoreleasePool* pool = nullptr;
    NS::SharedPtr<MTL::CommandBuffer> commands;
    std::unique_ptr<RenderPassEncoder> renderPass;
    std::unique_ptr<ComputePassEncoder> computePass;
    dispatch_semaphore_t frameSemaphore = nullptr;
    bool submitted = false;
};

struct Queue::Impl {
    NS::SharedPtr<MTL::CommandQueue> queue;
    dispatch_semaphore_t frameSemaphore = nullptr;
};

struct Device::Impl {
    NS::SharedPtr<MTL::Device> device;
    std::unique_ptr<Queue> queue;
};

Buffer::Buffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Buffer::~Buffer() = default;

std::uint64_t Buffer::size() const noexcept {
    return impl_->size;
}

Texture::Texture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Texture::~Texture() = default;

Extent2D Texture::extent() const noexcept {
    return impl_->extent;
}

TextureFormat Texture::format() const noexcept {
    return impl_->format;
}

std::uint32_t Texture::sampleCount() const noexcept {
    return impl_->sampleCount;
}

Sampler::Sampler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Sampler::~Sampler() = default;

ShaderModule::ShaderModule(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShaderModule::~ShaderModule() = default;

BindGroupLayout::BindGroupLayout(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BindGroupLayout::~BindGroupLayout() = default;

BindGroup::BindGroup(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BindGroup::~BindGroup() = default;

RenderPipeline::RenderPipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RenderPipeline::~RenderPipeline() = default;

ComputePipeline::ComputePipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ComputePipeline::~ComputePipeline() = default;

RenderPassEncoder::RenderPassEncoder() : impl_(std::make_unique<Impl>()) {}
RenderPassEncoder::~RenderPassEncoder() = default;

ComputePassEncoder::ComputePassEncoder() : impl_(std::make_unique<Impl>()) {}
ComputePassEncoder::~ComputePassEncoder() = default;

void RenderPassEncoder::setPipeline(RenderPipeline& pipeline) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(pipeline.impl_->sampleCount == impl_->passSampleCount);
    impl_->encoder->setRenderPipelineState(pipeline.impl_->state.get());
    if (pipeline.impl_->depthStencil) {
        impl_->encoder->setDepthStencilState(pipeline.impl_->depthStencil.get());
    }
    impl_->encoder->setCullMode(pipeline.impl_->cullMode);
    impl_->encoder->setFrontFacingWinding(pipeline.impl_->winding);
    impl_->encoder->setDepthBias(pipeline.impl_->depthBias, pipeline.impl_->depthBiasSlopeScale,
                                 pipeline.impl_->depthBiasClamp);
    impl_->primitive = pipeline.impl_->primitive;
}

void RenderPassEncoder::setBindGroup(std::uint32_t set, BindGroup& group) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(set < binding::kMaxBindGroups);
    if (set >= binding::kMaxBindGroups) {
        logError("setBindGroup: set {} exceeds Metal ABI limit", set);
        return;
    }

    for (const BindGroup::Impl::ResolvedEntry& resolved : group.impl_->resolved) {
        const BindGroupEntry& entry = resolved.entry;
        const std::uint32_t index = binding::resourceIndex(set, entry.binding);
        const bool vertex = hasFlag(resolved.visibility, ShaderStage::Vertex);
        const bool fragment = hasFlag(resolved.visibility, ShaderStage::Fragment);

        switch (resolved.type) {
        case BindingType::UniformBuffer:
        case BindingType::StorageBuffer:
            KUMO_ASSERT(entry.buffer != nullptr);
            if (!entry.buffer) {
                continue;
            }
            if (vertex) {
                impl_->encoder->setVertexBuffer(entry.buffer->impl_->buffer.get(),
                                                entry.bufferOffset, index);
            }
            if (fragment) {
                impl_->encoder->setFragmentBuffer(entry.buffer->impl_->buffer.get(),
                                                  entry.bufferOffset, index);
            }
            break;
        case BindingType::Texture:
        case BindingType::StorageTexture:
            KUMO_ASSERT(entry.texture != nullptr);
            if (!entry.texture) {
                continue;
            }
            if (vertex) {
                impl_->encoder->setVertexTexture(entry.texture->impl_->texture.get(), index);
            }
            if (fragment) {
                impl_->encoder->setFragmentTexture(entry.texture->impl_->texture.get(), index);
            }
            break;
        case BindingType::Sampler: {
            KUMO_ASSERT(entry.sampler != nullptr);
            if (!entry.sampler) {
                continue;
            }
            const std::uint32_t slot = binding::samplerIndex(set, entry.binding);
            if (slot > binding::kMaxSamplerIndex) {
                logError("setBindGroup: sampler set {} binding {} exceeds Metal slot {}", set,
                         entry.binding, binding::kMaxSamplerIndex);
                continue;
            }
            if (vertex) {
                impl_->encoder->setVertexSamplerState(entry.sampler->impl_->sampler.get(), slot);
            }
            if (fragment) {
                impl_->encoder->setFragmentSamplerState(entry.sampler->impl_->sampler.get(), slot);
            }
            break;
        }
        }
    }
}

void RenderPassEncoder::setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(slot < binding::kMaxVertexBufferSlots);
    KUMO_ASSERT(offset <= buffer.impl_->size);
    if (slot >= binding::kMaxVertexBufferSlots || offset > buffer.impl_->size) {
        logError("setVertexBuffer: invalid slot or offset");
        return;
    }
    impl_->encoder->setVertexBuffer(buffer.impl_->buffer.get(), offset,
                                    binding::vertexBufferIndex(slot));
}

void RenderPassEncoder::setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(offset <= buffer.impl_->size);
    if (offset > buffer.impl_->size) {
        logError("setIndexBuffer: offset exceeds buffer size");
        return;
    }
    impl_->indexBuffer = buffer.impl_->buffer.get();
    impl_->indexType = format == IndexFormat::Uint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
    impl_->indexOffset = offset;
}

void RenderPassEncoder::setPushConstants(ShaderStage stages, const void* data, std::uint32_t size) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(size <= kMaxPushConstantSize);
    KUMO_ASSERT(data != nullptr || size == 0);
    if (size > kMaxPushConstantSize || (data == nullptr && size != 0)) {
        logError("setPushConstants: invalid data or size {}", size);
        return;
    }
    if (hasFlag(stages, ShaderStage::Vertex)) {
        impl_->encoder->setVertexBytes(data, size, binding::kPushConstantBufferIndex);
    }
    if (hasFlag(stages, ShaderStage::Fragment)) {
        impl_->encoder->setFragmentBytes(data, size, binding::kPushConstantBufferIndex);
    }
}

void RenderPassEncoder::draw(std::uint32_t vertexCount, std::uint32_t instanceCount,
                             std::uint32_t firstVertex) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    impl_->encoder->drawPrimitives(impl_->primitive, static_cast<NS::UInteger>(firstVertex),
                                   static_cast<NS::UInteger>(vertexCount),
                                   static_cast<NS::UInteger>(instanceCount));
}

void RenderPassEncoder::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount,
                                    std::uint32_t firstIndex) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(impl_->indexBuffer != nullptr);
    if (impl_->indexBuffer == nullptr) {
        logError("drawIndexed: no index buffer is bound");
        return;
    }
    const std::uint64_t offset =
        impl_->indexOffset + static_cast<std::uint64_t>(firstIndex) *
                                 (impl_->indexType == MTL::IndexTypeUInt16 ? 2u : 4u);
    impl_->encoder->drawIndexedPrimitives(impl_->primitive, static_cast<NS::UInteger>(indexCount),
                                          impl_->indexType, impl_->indexBuffer, offset,
                                          static_cast<NS::UInteger>(instanceCount));
}

void RenderPassEncoder::end() {
    if (impl_->encoder && impl_->open) {
        impl_->encoder->endEncoding();
    }
    impl_->open = false;
}

void ComputePassEncoder::setPipeline(ComputePipeline& pipeline) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    impl_->encoder->setComputePipelineState(pipeline.impl_->state.get());
    impl_->threadsPerGroup = pipeline.impl_->threadsPerGroup;
}

void ComputePassEncoder::setBindGroup(std::uint32_t set, BindGroup& group) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(set < binding::kMaxBindGroups);
    if (set >= binding::kMaxBindGroups) {
        logError("setBindGroup: set {} exceeds Metal ABI limit", set);
        return;
    }

    for (const BindGroup::Impl::ResolvedEntry& resolved : group.impl_->resolved) {
        const BindGroupEntry& entry = resolved.entry;
        const std::uint32_t index = binding::resourceIndex(set, entry.binding);
        switch (resolved.type) {
        case BindingType::UniformBuffer:
        case BindingType::StorageBuffer:
            KUMO_ASSERT(entry.buffer != nullptr);
            if (entry.buffer) {
                impl_->encoder->setBuffer(entry.buffer->impl_->buffer.get(), entry.bufferOffset,
                                          index);
            }
            break;
        case BindingType::Texture:
        case BindingType::StorageTexture:
            KUMO_ASSERT(entry.texture != nullptr);
            if (entry.texture) {
                impl_->encoder->setTexture(entry.texture->impl_->texture.get(), index);
            }
            break;
        case BindingType::Sampler:
            KUMO_ASSERT(entry.sampler != nullptr);
            if (entry.sampler) {
                const std::uint32_t slot = binding::samplerIndex(set, entry.binding);
                if (slot > binding::kMaxSamplerIndex) {
                    logError("setBindGroup: sampler set {} binding {} exceeds Metal slot {}", set,
                             entry.binding, binding::kMaxSamplerIndex);
                    continue;
                }
                impl_->encoder->setSamplerState(entry.sampler->impl_->sampler.get(), slot);
            }
            break;
        }
    }
}

void ComputePassEncoder::setPushConstants(const void* data, std::uint32_t size) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    KUMO_ASSERT(size <= kMaxPushConstantSize);
    KUMO_ASSERT(data != nullptr || size == 0);
    if (size > kMaxPushConstantSize || (data == nullptr && size != 0)) {
        logError("setPushConstants: invalid data or size {}", size);
        return;
    }
    impl_->encoder->setBytes(data, size, binding::kPushConstantBufferIndex);
}

void ComputePassEncoder::dispatch(std::uint32_t groupsX, std::uint32_t groupsY,
                                  std::uint32_t groupsZ) {
    KUMO_ASSERT(impl_->open && impl_->encoder);
    impl_->encoder->dispatchThreadgroups(MTL::Size::Make(groupsX, groupsY, groupsZ),
                                         impl_->threadsPerGroup);
}

void ComputePassEncoder::end() {
    if (impl_->encoder && impl_->open) {
        impl_->encoder->endEncoding();
    }
    impl_->open = false;
}

SurfaceFrame::SurfaceFrame(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SurfaceFrame::~SurfaceFrame() = default;
SurfaceFrame::SurfaceFrame(SurfaceFrame&&) noexcept = default;
SurfaceFrame& SurfaceFrame::operator=(SurfaceFrame&&) noexcept = default;

Texture& SurfaceFrame::texture() noexcept {
    KUMO_ASSERT(impl_ && impl_->texture);
    return *impl_->texture;
}

const Texture& SurfaceFrame::texture() const noexcept {
    KUMO_ASSERT(impl_ && impl_->texture);
    return *impl_->texture;
}

Surface::Surface(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Surface::~Surface() = default;

void Surface::configure(const SurfaceConfig& config) {
    if (config.size.width == 0 || config.size.height == 0 ||
        toMtl(config.format) == MTL::PixelFormatInvalid) {
        logError("Surface::configure: invalid size or format");
        return;
    }
    impl_->format = config.format;
    impl_->extent = config.size;
    impl_->layer->setPixelFormat(toMtl(config.format));
    impl_->layer->setDrawableSize(CGSizeMake(static_cast<double>(config.size.width),
                                             static_cast<double>(config.size.height)));
}

std::optional<SurfaceFrame> Surface::acquire() {
    AutoreleasePoolGuard pool;
    NS::SharedPtr<CA::MetalDrawable> drawable = NS::RetainPtr(impl_->layer->nextDrawable());
    if (!drawable) {
        return std::nullopt;
    }

    auto textureImpl = std::make_unique<Texture::Impl>();
    textureImpl->texture = NS::RetainPtr(drawable->texture());
    textureImpl->extent = impl_->extent;
    textureImpl->format = impl_->format;
    textureImpl->sampleCount = 1;
    textureImpl->drawable = true;

    auto frameImpl = std::make_unique<SurfaceFrame::Impl>();
    frameImpl->drawable = std::move(drawable);
    frameImpl->texture.reset(new Texture(std::move(textureImpl)));
    SurfaceFrame frame(std::move(frameImpl));
    return std::optional<SurfaceFrame>(std::move(frame));
}

TextureFormat Surface::format() const noexcept {
    return impl_->format;
}

CommandEncoder::CommandEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    impl_->renderPass.reset(new RenderPassEncoder());
    impl_->computePass.reset(new ComputePassEncoder());
}

CommandEncoder::~CommandEncoder() {
    if (!impl_->submitted && impl_->frameSemaphore != nullptr) {
        dispatch_semaphore_signal(impl_->frameSemaphore);
    }
    if (impl_->pool != nullptr) {
        impl_->pool->release();
        impl_->pool = nullptr;
    }
}

RenderPassEncoder& CommandEncoder::beginRenderPass(const RenderPassDesc& desc) {
    KUMO_ASSERT(!impl_->submitted && impl_->commands);
    if (impl_->computePass->impl_->open) {
        impl_->computePass->end();
        impl_->computePass->impl_->reset();
    }
    if (impl_->renderPass->impl_->open) {
        impl_->renderPass->end();
    }
    impl_->renderPass->impl_->reset();

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    for (std::size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const RenderPassColorAttachment& attachment = desc.colorAttachments[i];
        KUMO_ASSERT(attachment.texture != nullptr);
        if (attachment.texture == nullptr) {
            logError("beginRenderPass: color attachment {} is null", i);
            continue;
        }
        MTL::RenderPassColorAttachmentDescriptor* color =
            pass->colorAttachments()->object(static_cast<NS::UInteger>(i));
        color->setTexture(attachment.texture->impl_->texture.get());
        if (i == 0) {
            impl_->renderPass->impl_->passSampleCount = attachment.texture->impl_->sampleCount;
        }
        color->setLoadAction(toMtl(attachment.loadOp));
        color->setStoreAction(toMtl(attachment.storeOp));
        color->setClearColor(MTL::ClearColor::Make(attachment.clearColor.r, attachment.clearColor.g,
                                                   attachment.clearColor.b,
                                                   attachment.clearColor.a));
        if (attachment.resolveTarget != nullptr) {
            color->setResolveTexture(attachment.resolveTarget->impl_->texture.get());
            color->setStoreAction(attachment.storeOp == StoreOp::Store
                                      ? MTL::StoreActionStoreAndMultisampleResolve
                                      : MTL::StoreActionMultisampleResolve);
        }
    }

    if (desc.depthAttachment.texture != nullptr) {
        Texture& target = *desc.depthAttachment.texture;
        if (desc.colorAttachments.empty()) {
            impl_->renderPass->impl_->passSampleCount = target.impl_->sampleCount;
        }
        MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
        depth->setTexture(target.impl_->texture.get());
        depth->setLoadAction(toMtl(desc.depthAttachment.loadOp));
        depth->setStoreAction(toMtl(desc.depthAttachment.storeOp));
        depth->setClearDepth(desc.depthAttachment.clearDepth);
    }

    impl_->renderPass->impl_->passDescriptor = NS::RetainPtr(pass);
    impl_->renderPass->impl_->encoder = NS::RetainPtr(impl_->commands->renderCommandEncoder(pass));
    KUMO_ASSERT(impl_->renderPass->impl_->encoder);
    impl_->renderPass->impl_->open = impl_->renderPass->impl_->encoder.get() != nullptr;
    return *impl_->renderPass;
}

ComputePassEncoder& CommandEncoder::beginComputePass() {
    KUMO_ASSERT(!impl_->submitted && impl_->commands);
    if (impl_->renderPass->impl_->open) {
        impl_->renderPass->end();
        impl_->renderPass->impl_->reset();
    }
    if (impl_->computePass->impl_->open) {
        impl_->computePass->end();
    }
    impl_->computePass->impl_->reset();
    impl_->computePass->impl_->encoder = NS::RetainPtr(impl_->commands->computeCommandEncoder());
    KUMO_ASSERT(impl_->computePass->impl_->encoder);
    impl_->computePass->impl_->open = impl_->computePass->impl_->encoder.get() != nullptr;
    return *impl_->computePass;
}

void CommandEncoder::generateMipmaps(Texture& texture) {
    KUMO_ASSERT(!impl_->submitted && impl_->commands);
    KUMO_ASSERT(!impl_->renderPass->impl_->open && !impl_->computePass->impl_->open);
    if (!isFilterable(texture.impl_->format)) {
        logError("generateMipmaps: format is not linear-filterable "
                 "(HDR mip chains should use RGBA16Float)");
        return;
    }
    if (texture.impl_->mipLevelCount <= 1) {
        return;
    }
    MTL::BlitCommandEncoder* blit = impl_->commands->blitCommandEncoder();
    if (blit == nullptr) {
        logError("generateMipmaps: blit encoder creation failed");
        return;
    }
    blit->generateMipmaps(texture.impl_->texture.get());
    blit->endEncoding();
}

bool CommandEncoder::copyTextureToBuffer(Texture& texture, Buffer& destination,
                                         std::uint64_t destinationOffset, std::uint64_t bytesPerRow,
                                         Extent2D size) {
    KUMO_ASSERT(!impl_->submitted && impl_->commands);
    KUMO_ASSERT(!impl_->renderPass->impl_->open && !impl_->computePass->impl_->open);
    std::uint64_t dataSize = 0;
    const std::uint64_t minimumBytesPerRow =
        static_cast<std::uint64_t>(size.width) * bytesPerPixel(texture.impl_->format);
    if (impl_->submitted || !impl_->commands || impl_->renderPass->impl_->open ||
        impl_->computePass->impl_->open || size.width == 0 || size.height == 0 ||
        texture.impl_->dimension != TextureDimension::Tex2D || texture.impl_->sampleCount != 1 ||
        size.width > texture.impl_->extent.width || size.height > texture.impl_->extent.height ||
        bytesPerRow < minimumBytesPerRow || !checkedDataSize(bytesPerRow, size, dataSize) ||
        destinationOffset > destination.impl_->size ||
        dataSize > destination.impl_->size - destinationOffset ||
        texture.impl_->texture->storageMode() == MTL::StorageModeMemoryless) {
        logError("copyTextureToBuffer: invalid texture or destination range");
        return false;
    }

    MTL::BlitCommandEncoder* blit = impl_->commands->blitCommandEncoder();
    if (blit == nullptr) {
        logError("copyTextureToBuffer: blit encoder creation failed");
        return false;
    }
    blit->copyFromTexture(texture.impl_->texture.get(), 0, 0, MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(size.width, size.height, 1),
                          destination.impl_->buffer.get(), destinationOffset, bytesPerRow, 0);
    blit->endEncoding();
    return true;
}

void CommandEncoder::finishAndSubmit(SurfaceFrame* presentFrame) {
    KUMO_ASSERT(!impl_->submitted && impl_->commands);
    if (impl_->submitted || !impl_->commands) {
        logError("finishAndSubmit: command encoder was already submitted");
        return;
    }

    if (impl_->renderPass->impl_->open) {
        impl_->renderPass->end();
    }
    if (impl_->computePass->impl_->open) {
        impl_->computePass->end();
    }

    if (presentFrame != nullptr) {
        KUMO_ASSERT(presentFrame->impl_ && presentFrame->impl_->drawable);
        KUMO_ASSERT(!presentFrame->impl_->presented);
        if (presentFrame->impl_ && presentFrame->impl_->drawable &&
            !presentFrame->impl_->presented) {
            impl_->commands->presentDrawable(presentFrame->impl_->drawable.get());
            presentFrame->impl_->presented = true;
        }
    }

    dispatch_semaphore_t semaphore = impl_->frameSemaphore;
    impl_->commands->addCompletedHandler(
        [semaphore](MTL::CommandBuffer*) { dispatch_semaphore_signal(semaphore); });
    impl_->commands->commit();
    impl_->submitted = true;

    // SurfaceFrame keeps the drawable alive through submission. Any drawable
    // readback must already have been blitted before present on this buffer.
    impl_->renderPass->impl_->reset();
    impl_->computePass->impl_->reset();
    impl_->commands.reset();
    impl_->pool->release();
    impl_->pool = nullptr;
}

Queue::Queue(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Queue::~Queue() {
    if (impl_->frameSemaphore != nullptr) {
        waitIdle();
        dispatch_release(impl_->frameSemaphore);
        impl_->frameSemaphore = nullptr;
    }
}

Ptr<CommandEncoder> Queue::createCommandEncoder() {
    dispatch_semaphore_wait(impl_->frameSemaphore, DISPATCH_TIME_FOREVER);

    auto encoderImpl = std::make_unique<CommandEncoder::Impl>();
    encoderImpl->pool = NS::AutoreleasePool::alloc()->init();
    encoderImpl->commands = NS::RetainPtr(impl_->queue->commandBuffer());
    encoderImpl->frameSemaphore = impl_->frameSemaphore;
    if (!encoderImpl->commands) {
        logError("createCommandEncoder: Metal command buffer creation failed");
        encoderImpl->pool->release();
        encoderImpl->pool = nullptr;
        dispatch_semaphore_signal(impl_->frameSemaphore);
        return nullptr;
    }
    return Ptr<CommandEncoder>(new CommandEncoder(std::move(encoderImpl)));
}

void Queue::waitIdle() {
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_wait(impl_->frameSemaphore, DISPATCH_TIME_FOREVER);
    }
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_signal(impl_->frameSemaphore);
    }
}

void Queue::writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                        std::uint64_t size) {
    if ((data == nullptr && size != 0) || offset > buffer.impl_->size ||
        size > buffer.impl_->size - offset) {
        logError("writeBuffer: invalid data range");
        return;
    }
    std::memcpy(static_cast<char*>(buffer.impl_->buffer->contents()) + offset, data, size);
}

bool Queue::readBuffer(Buffer& buffer, std::uint64_t offset, void* out, std::uint64_t size) {
    if ((out == nullptr && size != 0) || offset > buffer.impl_->size ||
        size > buffer.impl_->size - offset) {
        logError("readBuffer: invalid output range");
        return false;
    }

    // A zero-work command buffer serializes the CPU read after all prior work
    // on this Metal queue without consuming a frame-in-flight slot.
    AutoreleasePoolGuard pool;
    MTL::CommandBuffer* commands = impl_->queue->commandBuffer();
    if (commands == nullptr) {
        logError("readBuffer: command buffer creation failed");
        return false;
    }
    commands->commit();
    commands->waitUntilCompleted();
    if (commands->status() == MTL::CommandBufferStatusError) {
        logError("readBuffer: synchronization command buffer failed");
        return false;
    }
    std::memcpy(out, static_cast<const char*>(buffer.impl_->buffer->contents()) + offset, size);
    return true;
}

void Queue::writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                         Extent2D size) {
    if (data == nullptr || size.width == 0 || size.height == 0 ||
        size.width > texture.impl_->extent.width || size.height > texture.impl_->extent.height) {
        logError("writeTexture: invalid data or extent");
        return;
    }
    if (texture.impl_->texture->storageMode() == MTL::StorageModeMemoryless) {
        logError("writeTexture: memoryless textures cannot be uploaded");
        return;
    }
    texture.impl_->texture->replaceRegion(MTL::Region::Make2D(0, 0, size.width, size.height), 0,
                                          data, bytesPerRow);
}

bool Queue::readTexture(Texture& texture, void* out, std::uint64_t bytesPerRow, Extent2D size) {
    std::uint64_t dataSize = 0;
    if (out == nullptr || size.width == 0 || size.height == 0 || bytesPerRow == 0 ||
        size.width > texture.impl_->extent.width || size.height > texture.impl_->extent.height ||
        !checkedDataSize(bytesPerRow, size, dataSize)) {
        logError("readTexture: invalid output range");
        return false;
    }
    if (texture.impl_->texture->storageMode() == MTL::StorageModeMemoryless) {
        logError("readTexture: memoryless textures cannot be read back");
        return false;
    }
    if (texture.impl_->drawable) {
        logError("readTexture: drawable readback must be encoded before presentation");
        return false;
    }

    AutoreleasePoolGuard pool;
    NS::SharedPtr<MTL::Buffer> staging = NS::TransferPtr(
        impl_->queue->device()->newBuffer(dataSize, MTL::ResourceStorageModeShared));
    if (!staging) {
        logError("readTexture: staging buffer allocation failed ({} bytes)", dataSize);
        return false;
    }
    MTL::CommandBuffer* commands = impl_->queue->commandBuffer();
    if (commands == nullptr) {
        logError("readTexture: command buffer creation failed");
        return false;
    }
    MTL::BlitCommandEncoder* blit = commands->blitCommandEncoder();
    if (blit == nullptr) {
        logError("readTexture: blit encoder creation failed");
        return false;
    }
    blit->copyFromTexture(texture.impl_->texture.get(), 0, 0, MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(size.width, size.height, 1), staging.get(), 0,
                          bytesPerRow, 0);
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    if (commands->status() == MTL::CommandBufferStatusError) {
        logError("readTexture: blit command buffer failed");
        return false;
    }
    std::memcpy(out, staging->contents(), dataSize);
    return true;
}

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    NS::SharedPtr<MTL::CommandQueue> commandQueue =
        NS::TransferPtr(impl_->device->newCommandQueue());
    if (!commandQueue) {
        logError("Metal command queue creation failed");
        return;
    }
    auto queueImpl = std::make_unique<Queue::Impl>();
    queueImpl->queue = std::move(commandQueue);
    queueImpl->frameSemaphore = dispatch_semaphore_create(kFramesInFlight);
    if (queueImpl->frameSemaphore == nullptr) {
        logError("frame semaphore creation failed");
        return;
    }
    impl_->queue.reset(new Queue(std::move(queueImpl)));
}

Device::~Device() = default;

Ptr<Buffer> Device::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        logError("createBuffer: size must be greater than zero");
        return nullptr;
    }
    NS::SharedPtr<MTL::Buffer> buffer =
        NS::TransferPtr(impl_->device->newBuffer(desc.size, MTL::ResourceStorageModeShared));
    if (!buffer) {
        logError("createBuffer failed (size {})", desc.size);
        return nullptr;
    }
    auto bufferImpl = std::make_unique<Buffer::Impl>();
    bufferImpl->buffer = std::move(buffer);
    bufferImpl->size = desc.size;
    return Ptr<Buffer>(new Buffer(std::move(bufferImpl)));
}

Ptr<Texture> Device::createTexture(const TextureDesc& desc) {
    if (desc.size.width == 0 || desc.size.height == 0 || desc.format == TextureFormat::Undefined ||
        desc.mipLevelCount == 0 || (desc.sampleCount != 1 && desc.sampleCount != 4)) {
        logError("createTexture: invalid size, format, mip count, or sample count");
        return nullptr;
    }
    const bool multisample = desc.sampleCount > 1;
    if ((multisample && desc.mipLevelCount != 1) ||
        (multisample && desc.dimension != TextureDimension::Tex2D)) {
        logError("createTexture: multisample textures must be 2D with one mip level");
        return nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(desc.dimension == TextureDimension::Cube ? MTL::TextureTypeCube
                               : multisample ? MTL::TextureType2DMultisample
                                             : MTL::TextureType2D);
    descriptor->setPixelFormat(toMtl(desc.format));
    descriptor->setWidth(desc.size.width);
    descriptor->setHeight(desc.size.height);
    descriptor->setMipmapLevelCount(desc.mipLevelCount);
    descriptor->setSampleCount(desc.sampleCount);

    MTL::TextureUsage usage = 0;
    if (hasFlag(desc.usage, TextureUsage::Sampled)) {
        usage |= MTL::TextureUsageShaderRead;
    }
    if (hasFlag(desc.usage, TextureUsage::RenderTarget)) {
        usage |= MTL::TextureUsageRenderTarget;
    }
    if (hasFlag(desc.usage, TextureUsage::Storage)) {
        usage |= MTL::TextureUsageShaderWrite;
    }
    descriptor->setUsage(usage);

    MTL::StorageMode storageMode = MTL::StorageModeShared;
    if (desc.storageMode == StorageMode::TransientAttachment) {
        storageMode = MTL::StorageModePrivate;
        if (isTransientDescriptorValid(desc) && supportsMemoryless(impl_->device.get())) {
            storageMode = MTL::StorageModeMemoryless;
        } else {
            logDebug("transient texture {}x{} uses private-storage fallback", desc.size.width,
                     desc.size.height);
        }
    } else if (multisample || (hasFlag(desc.usage, TextureUsage::RenderTarget) &&
                               !hasFlag(desc.usage, TextureUsage::CopyDst) &&
                               !hasFlag(desc.usage, TextureUsage::CopySrc))) {
        storageMode = MTL::StorageModePrivate;
    }
    descriptor->setStorageMode(storageMode);

    NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(impl_->device->newTexture(descriptor));
    descriptor->release();
    if (!texture) {
        logError("createTexture failed ({}x{})", desc.size.width, desc.size.height);
        return nullptr;
    }

    auto textureImpl = std::make_unique<Texture::Impl>();
    textureImpl->texture = std::move(texture);
    textureImpl->extent = desc.size;
    textureImpl->format = desc.format;
    textureImpl->dimension = desc.dimension;
    textureImpl->mipLevelCount = desc.mipLevelCount;
    textureImpl->arrayLayerCount = desc.dimension == TextureDimension::Cube ? 6u : 1u;
    textureImpl->sampleCount = desc.sampleCount;
    return Ptr<Texture>(new Texture(std::move(textureImpl)));
}

Ptr<Texture> Device::createTextureView(const Ptr<Texture>& texture, const TextureViewDesc& desc) {
    if (!texture || desc.mipLevelCount == 0 || desc.arrayLayerCount == 0 ||
        desc.baseMipLevel >= texture->impl_->mipLevelCount ||
        desc.mipLevelCount > texture->impl_->mipLevelCount - desc.baseMipLevel ||
        desc.baseArrayLayer >= texture->impl_->arrayLayerCount ||
        desc.arrayLayerCount > texture->impl_->arrayLayerCount - desc.baseArrayLayer) {
        logError("createTextureView: invalid texture or subresource range");
        return nullptr;
    }

    const MTL::TextureType type =
        desc.dimension == TextureDimension::Cube ? MTL::TextureTypeCube : MTL::TextureType2D;
    NS::SharedPtr<MTL::Texture> view = NS::TransferPtr(texture->impl_->texture->newTextureView(
        texture->impl_->texture->pixelFormat(), type,
        NS::Range::Make(desc.baseMipLevel, desc.mipLevelCount),
        NS::Range::Make(desc.baseArrayLayer, desc.arrayLayerCount)));
    if (!view) {
        logError("createTextureView failed");
        return nullptr;
    }

    const Extent2D base = texture->impl_->extent;
    auto viewImpl = std::make_unique<Texture::Impl>();
    viewImpl->texture = std::move(view);
    viewImpl->parent = texture;
    viewImpl->extent = {std::max(1u, base.width >> desc.baseMipLevel),
                        std::max(1u, base.height >> desc.baseMipLevel)};
    viewImpl->format = texture->impl_->format;
    viewImpl->dimension = desc.dimension;
    viewImpl->mipLevelCount = desc.mipLevelCount;
    viewImpl->arrayLayerCount = desc.arrayLayerCount;
    viewImpl->sampleCount = texture->impl_->sampleCount;
    return Ptr<Texture>(new Texture(std::move(viewImpl)));
}

Ptr<Sampler> Device::createSampler(const SamplerDesc& desc) {
    if (desc.maxAnisotropy == 0) {
        logError("createSampler: maxAnisotropy must be greater than zero");
        return nullptr;
    }
    MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
    descriptor->setMagFilter(toMtl(desc.magFilter));
    descriptor->setMinFilter(toMtl(desc.minFilter));
    descriptor->setMipFilter(desc.mipFilter == FilterMode::Nearest ? MTL::SamplerMipFilterNearest
                                                                   : MTL::SamplerMipFilterLinear);
    descriptor->setSAddressMode(toMtl(desc.addressModeU));
    descriptor->setTAddressMode(toMtl(desc.addressModeV));
    descriptor->setRAddressMode(toMtl(desc.addressModeW));
    descriptor->setLodMaxClamp(desc.lodMaxClamp);
    descriptor->setMaxAnisotropy(desc.maxAnisotropy);
    descriptor->setCompareFunction(toMtl(desc.compare));

    NS::SharedPtr<MTL::SamplerState> sampler =
        NS::TransferPtr(impl_->device->newSamplerState(descriptor));
    descriptor->release();
    if (!sampler) {
        logError("createSampler failed");
        return nullptr;
    }
    auto samplerImpl = std::make_unique<Sampler::Impl>();
    samplerImpl->sampler = std::move(sampler);
    return Ptr<Sampler>(new Sampler(std::move(samplerImpl)));
}

Ptr<ShaderModule> Device::createShaderModule(const ShaderModuleDesc& desc) {
    if (!isSingleShaderStage(desc.stage) || desc.mslSource.empty() || desc.entryPoint.empty()) {
        logError("createShaderModule: stage, MSL source, and entry point are required");
        return nullptr;
    }
    AutoreleasePoolGuard pool;
    NS::Error* error = nullptr;
    NS::SharedPtr<MTL::Library> library =
        NS::TransferPtr(impl_->device->newLibrary(nsString(desc.mslSource), nullptr, &error));
    if (!library) {
        logError("MSL compile failed: {}",
                 error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
        return nullptr;
    }
    NS::SharedPtr<MTL::Function> function =
        NS::TransferPtr(library->newFunction(nsString(desc.entryPoint)));
    if (!function) {
        logError("entry point '{}' not found", desc.entryPoint);
        return nullptr;
    }
    auto shaderImpl = std::make_unique<ShaderModule::Impl>();
    shaderImpl->library = std::move(library);
    shaderImpl->function = std::move(function);
    shaderImpl->stage = desc.stage;
    return Ptr<ShaderModule>(new ShaderModule(std::move(shaderImpl)));
}

Ptr<BindGroupLayout> Device::createBindGroupLayout(const BindGroupLayoutDesc& desc) {
    for (std::size_t i = 0; i < desc.entries.size(); ++i) {
        const BindGroupLayoutEntry& entry = desc.entries[i];
        if (entry.binding >= binding::kMaxBindingsPerSet || entry.visibility == ShaderStage::None) {
            logError("createBindGroupLayout: invalid binding {}", entry.binding);
            return nullptr;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (desc.entries[j].binding == entry.binding) {
                logError("createBindGroupLayout: duplicate binding {}", entry.binding);
                return nullptr;
            }
        }
    }
    auto layoutImpl = std::make_unique<BindGroupLayout::Impl>();
    layoutImpl->desc = desc;
    return Ptr<BindGroupLayout>(new BindGroupLayout(std::move(layoutImpl)));
}

Ptr<BindGroup> Device::createBindGroup(const BindGroupDesc& desc) {
    if (!desc.layout) {
        logError("createBindGroup: layout is required");
        return nullptr;
    }
    const std::vector<BindGroupLayoutEntry>& layoutEntries = desc.layout->impl_->desc.entries;
    if (desc.entries.size() != layoutEntries.size()) {
        logError("createBindGroup: expected {} entries, received {}", layoutEntries.size(),
                 desc.entries.size());
        return nullptr;
    }

    auto groupImpl = std::make_unique<BindGroup::Impl>();
    std::vector<bool> seen(binding::kMaxBindingsPerSet, false);
    for (const BindGroupEntry& entry : desc.entries) {
        const BindGroupLayoutEntry* match = nullptr;
        for (const BindGroupLayoutEntry& layoutEntry : layoutEntries) {
            if (layoutEntry.binding == entry.binding) {
                match = &layoutEntry;
                break;
            }
        }
        if (match == nullptr) {
            logError("createBindGroup: binding {} is absent from the layout", entry.binding);
            return nullptr;
        }
        if (seen[entry.binding]) {
            logError("createBindGroup: duplicate binding {}", entry.binding);
            return nullptr;
        }
        seen[entry.binding] = true;

        const bool wantsBuffer =
            match->type == BindingType::UniformBuffer || match->type == BindingType::StorageBuffer;
        const bool wantsTexture =
            match->type == BindingType::Texture || match->type == BindingType::StorageTexture;
        const bool wantsSampler = match->type == BindingType::Sampler;
        const std::uint32_t resourceCount = static_cast<std::uint32_t>(entry.buffer != nullptr) +
                                            static_cast<std::uint32_t>(entry.texture != nullptr) +
                                            static_cast<std::uint32_t>(entry.sampler != nullptr);
        if (resourceCount != 1 || wantsBuffer != (entry.buffer != nullptr) ||
            wantsTexture != (entry.texture != nullptr) ||
            wantsSampler != (entry.sampler != nullptr) ||
            (!wantsBuffer && entry.bufferOffset != 0)) {
            logError("createBindGroup: binding {} has the wrong resource type", entry.binding);
            return nullptr;
        }
        if (wantsBuffer && entry.bufferOffset > entry.buffer->impl_->size) {
            logError("createBindGroup: binding {} buffer offset is out of range", entry.binding);
            return nullptr;
        }
        groupImpl->resolved.push_back({entry, match->visibility, match->type});
    }
    return Ptr<BindGroup>(new BindGroup(std::move(groupImpl)));
}

Ptr<RenderPipeline> Device::createRenderPipeline(const RenderPipelineDesc& desc) {
    if (!desc.vertexShader || !desc.fragmentShader ||
        desc.vertexShader->impl_->stage != ShaderStage::Vertex ||
        desc.fragmentShader->impl_->stage != ShaderStage::Fragment) {
        logError("createRenderPipeline requires vertex and fragment shader modules");
        return nullptr;
    }
    if (desc.vertexBuffers.size() > binding::kMaxVertexBufferSlots ||
        desc.bindGroupLayouts.size() > binding::kMaxBindGroups ||
        desc.pushConstantSize > kMaxPushConstantSize ||
        (desc.sampleCount != 1 && desc.sampleCount != 4)) {
        logError("createRenderPipeline: descriptor exceeds a Metal ABI limit");
        return nullptr;
    }
    for (std::size_t set = 0; set < desc.bindGroupLayouts.size(); ++set) {
        const Ptr<BindGroupLayout>& layout = desc.bindGroupLayouts[set];
        if (layout && !layoutFitsMetalSet(layout->impl_->desc, static_cast<std::uint32_t>(set))) {
            logError("createRenderPipeline: bind group layout {} exceeds Metal sampler slots", set);
            return nullptr;
        }
    }
    AutoreleasePoolGuard pool;

    MTL::RenderPipelineDescriptor* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(desc.vertexShader->impl_->function.get());
    descriptor->setFragmentFunction(desc.fragmentShader->impl_->function.get());

    if (!desc.vertexBuffers.empty()) {
        MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::vertexDescriptor();
        for (std::size_t slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
            const VertexBufferLayout& layout = desc.vertexBuffers[slot];
            const std::uint32_t bufferIndex =
                binding::vertexBufferIndex(static_cast<std::uint32_t>(slot));
            MTL::VertexBufferLayoutDescriptor* layoutDescriptor =
                vertexDescriptor->layouts()->object(bufferIndex);
            layoutDescriptor->setStride(layout.stride);
            layoutDescriptor->setStepFunction(layout.stepMode == VertexStepMode::Vertex
                                                  ? MTL::VertexStepFunctionPerVertex
                                                  : MTL::VertexStepFunctionPerInstance);
            for (const VertexAttribute& attribute : layout.attributes) {
                MTL::VertexAttributeDescriptor* attributeDescriptor =
                    vertexDescriptor->attributes()->object(attribute.shaderLocation);
                attributeDescriptor->setFormat(toMtl(attribute.format));
                attributeDescriptor->setOffset(attribute.offset);
                attributeDescriptor->setBufferIndex(bufferIndex);
            }
        }
        descriptor->setVertexDescriptor(vertexDescriptor);
    }

    for (std::size_t i = 0; i < desc.colorFormats.size(); ++i) {
        descriptor->colorAttachments()
            ->object(static_cast<NS::UInteger>(i))
            ->setPixelFormat(toMtl(desc.colorFormats[i]));
    }
    if (desc.depthStencil.format != TextureFormat::Undefined) {
        descriptor->setDepthAttachmentPixelFormat(toMtl(desc.depthStencil.format));
    }
    descriptor->setRasterSampleCount(desc.sampleCount);

    NS::Error* error = nullptr;
    NS::SharedPtr<MTL::RenderPipelineState> state =
        NS::TransferPtr(impl_->device->newRenderPipelineState(descriptor, &error));
    descriptor->release();
    if (!state) {
        logError("createRenderPipeline failed: {}",
                 error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
        return nullptr;
    }

    NS::SharedPtr<MTL::DepthStencilState> depthStencil;
    if (desc.depthStencil.format != TextureFormat::Undefined) {
        MTL::DepthStencilDescriptor* depthDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
        depthDescriptor->setDepthCompareFunction(toMtl(desc.depthStencil.depthCompare));
        depthDescriptor->setDepthWriteEnabled(desc.depthStencil.depthWriteEnabled);
        depthStencil = NS::TransferPtr(impl_->device->newDepthStencilState(depthDescriptor));
        depthDescriptor->release();
        if (!depthStencil) {
            logError("createRenderPipeline: depth-stencil state creation failed");
            return nullptr;
        }
    }

    auto pipelineImpl = std::make_unique<RenderPipeline::Impl>();
    pipelineImpl->state = std::move(state);
    pipelineImpl->depthStencil = std::move(depthStencil);
    pipelineImpl->primitive = toMtlPrimitive(desc.topology);
    pipelineImpl->cullMode = desc.cullMode == CullMode::None    ? MTL::CullModeNone
                             : desc.cullMode == CullMode::Front ? MTL::CullModeFront
                                                                : MTL::CullModeBack;
    pipelineImpl->winding =
        desc.frontFace == FrontFace::CCW ? MTL::WindingCounterClockwise : MTL::WindingClockwise;
    pipelineImpl->sampleCount = desc.sampleCount;
    pipelineImpl->depthBias = desc.depthStencil.depthBias;
    pipelineImpl->depthBiasSlopeScale = desc.depthStencil.depthBiasSlopeScale;
    pipelineImpl->depthBiasClamp = desc.depthStencil.depthBiasClamp;
    return Ptr<RenderPipeline>(new RenderPipeline(std::move(pipelineImpl)));
}

Ptr<ComputePipeline> Device::createComputePipeline(const ComputePipelineDesc& desc) {
    if (!desc.shader || desc.shader->impl_->stage != ShaderStage::Compute) {
        logError("createComputePipeline requires a compute shader module");
        return nullptr;
    }
    if (desc.bindGroupLayouts.size() > binding::kMaxBindGroups ||
        desc.pushConstantSize > kMaxPushConstantSize || desc.workgroupSizeX == 0 ||
        desc.workgroupSizeY == 0 || desc.workgroupSizeZ == 0) {
        logError("createComputePipeline: invalid descriptor or Metal ABI limit");
        return nullptr;
    }
    for (std::size_t set = 0; set < desc.bindGroupLayouts.size(); ++set) {
        const Ptr<BindGroupLayout>& layout = desc.bindGroupLayouts[set];
        if (layout && !layoutFitsMetalSet(layout->impl_->desc, static_cast<std::uint32_t>(set))) {
            logError("createComputePipeline: bind group layout {} exceeds Metal sampler slots",
                     set);
            return nullptr;
        }
    }
    AutoreleasePoolGuard pool;
    NS::Error* error = nullptr;
    NS::SharedPtr<MTL::ComputePipelineState> state = NS::TransferPtr(
        impl_->device->newComputePipelineState(desc.shader->impl_->function.get(), &error));
    if (!state) {
        logError("createComputePipeline failed: {}",
                 error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
        return nullptr;
    }
    auto pipelineImpl = std::make_unique<ComputePipeline::Impl>();
    pipelineImpl->state = std::move(state);
    pipelineImpl->threadsPerGroup =
        MTL::Size::Make(desc.workgroupSizeX, desc.workgroupSizeY, desc.workgroupSizeZ);
    return Ptr<ComputePipeline>(new ComputePipeline(std::move(pipelineImpl)));
}

Queue& Device::queue() noexcept {
    KUMO_ASSERT(impl_->queue);
    return *impl_->queue;
}

Ptr<Device> createDevice() {
    AutoreleasePoolGuard pool;
    NS::SharedPtr<MTL::Device> native = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!native) {
        logError("no Metal device available");
        return nullptr;
    }
    logInfo("Metal device: {}", native->name()->utf8String());
    auto deviceImpl = std::make_unique<Device::Impl>();
    deviceImpl->device = std::move(native);
    Ptr<Device> device(new Device(std::move(deviceImpl)));
    if (!device->impl_->queue) {
        return nullptr;
    }
    return device;
}

namespace detail {

struct MetalInterop {
    static Ptr<Surface> createSurface(Device& device, CA::MetalLayer* layer,
                                      bool allowDrawableReadback) {
        if (layer == nullptr) {
            logError("createSurface: CAMetalLayer is null");
            return nullptr;
        }
        auto surfaceImpl = std::make_unique<Surface::Impl>();
        surfaceImpl->layer = NS::RetainPtr(layer);
        surfaceImpl->layer->setDevice(device.impl_->device.get());
        surfaceImpl->layer->setFramebufferOnly(!allowDrawableReadback);
        return Ptr<Surface>(new Surface(std::move(surfaceImpl)));
    }

    static MTL::Device* nativeDevice(Device& device) noexcept { return device.impl_->device.get(); }

    static MTL::CommandBuffer* nativeCommandBuffer(CommandEncoder& encoder) noexcept {
        return encoder.impl_->commands.get();
    }

    static MTL::RenderCommandEncoder*
    nativeRenderCommandEncoder(RenderPassEncoder& encoder) noexcept {
        return encoder.impl_->encoder.get();
    }

    static MTL::RenderPassDescriptor*
    nativeRenderPassDescriptor(RenderPassEncoder& encoder) noexcept {
        return encoder.impl_->passDescriptor.get();
    }
};

} // namespace detail

namespace metal {

Ptr<Surface> createSurface(Device& device, CA::MetalLayer* layer, bool allowDrawableReadback) {
    return detail::MetalInterop::createSurface(device, layer, allowDrawableReadback);
}

MTL::Device* nativeDevice(Device& device) noexcept {
    return detail::MetalInterop::nativeDevice(device);
}

MTL::CommandBuffer* nativeCommandBuffer(CommandEncoder& encoder) noexcept {
    return detail::MetalInterop::nativeCommandBuffer(encoder);
}

MTL::RenderCommandEncoder* nativeRenderCommandEncoder(RenderPassEncoder& encoder) noexcept {
    return detail::MetalInterop::nativeRenderCommandEncoder(encoder);
}

MTL::RenderPassDescriptor* nativeRenderPassDescriptor(RenderPassEncoder& encoder) noexcept {
    return detail::MetalInterop::nativeRenderPassDescriptor(encoder);
}

} // namespace metal

} // namespace kumo::gpu
