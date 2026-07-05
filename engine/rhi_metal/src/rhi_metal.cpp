#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "kumo/rhi_metal/rhi_metal.h"

#include "kumo/core/assert.h"
#include "kumo/core/log.h"
#include "kumo/rhi_metal/binding_map.h"

#include <dispatch/dispatch.h>

#include <cstring>
#include <vector>

namespace kumo::rhi::metal {

namespace {

constexpr std::uint32_t kFramesInFlight = 2;

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

// Resource creation runs outside the per-frame pool; autoreleased temporaries
// (NS::String, descriptors, NS::Error) would otherwise leak on repeated use.
struct AutoreleasePoolGuard {
    AutoreleasePoolGuard() : pool(NS::AutoreleasePool::alloc()->init()) {}
    ~AutoreleasePoolGuard() { pool->release(); }
    AutoreleasePoolGuard(const AutoreleasePoolGuard&) = delete;
    AutoreleasePoolGuard& operator=(const AutoreleasePoolGuard&) = delete;
    NS::AutoreleasePool* pool;
};

class MetalBuffer final : public Buffer {
public:
    MetalBuffer(NS::SharedPtr<MTL::Buffer> buffer, std::uint64_t size)
        : buffer_(std::move(buffer)), size_(size) {}

    std::uint64_t size() const override { return size_; }
    MTL::Buffer* handle() const { return buffer_.get(); }

private:
    NS::SharedPtr<MTL::Buffer> buffer_;
    std::uint64_t size_ = 0;
};

class MetalTexture final : public Texture {
public:
    MetalTexture(NS::SharedPtr<MTL::Texture> texture, const TextureDesc& desc)
        : owned_(std::move(texture)), extent_(desc.size), format_(desc.format) {}

    // Transient wrapper around a surface drawable's texture.
    MetalTexture() = default;

    void reset(MTL::Texture* borrowed, Extent2D extent, TextureFormat format) {
        borrowed_ = borrowed;
        extent_ = extent;
        format_ = format;
    }

    Extent2D extent() const override { return extent_; }
    TextureFormat format() const override { return format_; }
    MTL::Texture* handle() const { return borrowed_ ? borrowed_ : owned_.get(); }

private:
    NS::SharedPtr<MTL::Texture> owned_;
    MTL::Texture* borrowed_ = nullptr;
    Extent2D extent_;
    TextureFormat format_ = TextureFormat::Undefined;
};

class MetalSampler final : public Sampler {
public:
    explicit MetalSampler(NS::SharedPtr<MTL::SamplerState> sampler)
        : sampler_(std::move(sampler)) {}
    MTL::SamplerState* handle() const { return sampler_.get(); }

private:
    NS::SharedPtr<MTL::SamplerState> sampler_;
};

class MetalShaderModule final : public ShaderModule {
public:
    MetalShaderModule(NS::SharedPtr<MTL::Library> library, NS::SharedPtr<MTL::Function> function)
        : library_(std::move(library)), function_(std::move(function)) {}

    MTL::Function* function() const { return function_.get(); }

private:
    NS::SharedPtr<MTL::Library> library_;
    NS::SharedPtr<MTL::Function> function_;
};

class MetalBindGroupLayout final : public BindGroupLayout {
public:
    explicit MetalBindGroupLayout(BindGroupLayoutDesc desc) : desc_(std::move(desc)) {}
    const BindGroupLayoutDesc& desc() const { return desc_; }

private:
    BindGroupLayoutDesc desc_;
};

class MetalBindGroup final : public BindGroup {
public:
    MetalBindGroup(const BindGroupDesc& desc, const MetalBindGroupLayout& layout) {
        for (const BindGroupEntry& entry : desc.entries) {
            const BindGroupLayoutEntry* match = nullptr;
            for (const BindGroupLayoutEntry& layoutEntry : layout.desc().entries) {
                if (layoutEntry.binding == entry.binding) {
                    match = &layoutEntry;
                    break;
                }
            }
            KUMO_ASSERT(match != nullptr);
            if (match != nullptr) {
                resolved_.push_back({entry, match->visibility, match->type});
            }
        }
    }

    void apply(MTL::RenderCommandEncoder* encoder, std::uint32_t set) const {
        for (const ResolvedEntry& resolved : resolved_) {
            const BindGroupEntry& entry = resolved.entry;
            const std::uint32_t index = resourceIndex(set, entry.binding);
            const bool vertex = hasFlag(resolved.visibility, ShaderStage::Vertex);
            const bool fragment = hasFlag(resolved.visibility, ShaderStage::Fragment);

            switch (resolved.type) {
            case BindingType::UniformBuffer:
            case BindingType::StorageBuffer: {
                auto* buffer = static_cast<MetalBuffer*>(entry.buffer.get());
                KUMO_ASSERT(buffer != nullptr);
                if (vertex) {
                    encoder->setVertexBuffer(buffer->handle(), entry.bufferOffset, index);
                }
                if (fragment) {
                    encoder->setFragmentBuffer(buffer->handle(), entry.bufferOffset, index);
                }
                break;
            }
            case BindingType::Texture: {
                auto* texture = static_cast<MetalTexture*>(entry.texture.get());
                KUMO_ASSERT(texture != nullptr);
                if (vertex) {
                    encoder->setVertexTexture(texture->handle(), index);
                }
                if (fragment) {
                    encoder->setFragmentTexture(texture->handle(), index);
                }
                break;
            }
            case BindingType::Sampler: {
                auto* sampler = static_cast<MetalSampler*>(entry.sampler.get());
                KUMO_ASSERT(sampler != nullptr);
                if (vertex) {
                    encoder->setVertexSamplerState(sampler->handle(), index);
                }
                if (fragment) {
                    encoder->setFragmentSamplerState(sampler->handle(), index);
                }
                break;
            }
            }
        }
    }

private:
    struct ResolvedEntry {
        BindGroupEntry entry;
        ShaderStage visibility;
        BindingType type;
    };

    std::vector<ResolvedEntry> resolved_;
};

class MetalRenderPipeline final : public RenderPipeline {
public:
    MetalRenderPipeline(NS::SharedPtr<MTL::RenderPipelineState> state,
                        NS::SharedPtr<MTL::DepthStencilState> depthStencil,
                        const RenderPipelineDesc& desc)
        : state_(std::move(state)), depthStencil_(std::move(depthStencil)),
          primitive_(toMtlPrimitive(desc.topology)),
          cullMode_(desc.cullMode == CullMode::None    ? MTL::CullModeNone
                    : desc.cullMode == CullMode::Front ? MTL::CullModeFront
                                                       : MTL::CullModeBack),
          winding_(desc.frontFace == FrontFace::CCW ? MTL::WindingCounterClockwise
                                                    : MTL::WindingClockwise) {}

    MTL::RenderPipelineState* state() const { return state_.get(); }
    MTL::DepthStencilState* depthStencil() const { return depthStencil_.get(); }
    MTL::PrimitiveType primitive() const { return primitive_; }
    MTL::CullMode cullMode() const { return cullMode_; }
    MTL::Winding winding() const { return winding_; }

private:
    NS::SharedPtr<MTL::RenderPipelineState> state_;
    NS::SharedPtr<MTL::DepthStencilState> depthStencil_;
    MTL::PrimitiveType primitive_;
    MTL::CullMode cullMode_;
    MTL::Winding winding_;
};

class MetalSurface final : public Surface {
public:
    explicit MetalSurface(CA::MetalLayer* layer, MTL::Device* device) : layer_(layer) {
        layer_->setDevice(device);
        layer_->setFramebufferOnly(true);
    }

    void configure(const SurfaceConfig& config) override {
        format_ = config.format;
        layer_->setPixelFormat(toMtl(config.format));
        layer_->setDrawableSize(CGSizeMake(static_cast<double>(config.size.width),
                                           static_cast<double>(config.size.height)));
        extent_ = config.size;
    }

    Texture* acquireNextTexture() override {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        drawable_ = NS::RetainPtr(layer_->nextDrawable());
        pool->release();
        if (!drawable_) {
            return nullptr;
        }
        texture_.reset(drawable_->texture(), extent_, format_);
        return &texture_;
    }

    TextureFormat format() const override { return format_; }

    CA::MetalDrawable* currentDrawable() const { return drawable_.get(); }
    void releaseDrawable() { drawable_.reset(); }

private:
    CA::MetalLayer* layer_ = nullptr;
    NS::SharedPtr<CA::MetalDrawable> drawable_;
    MetalTexture texture_;
    Extent2D extent_;
    TextureFormat format_ = TextureFormat::BGRA8Unorm;
};

class MetalRenderPassEncoder final : public RenderPassEncoder {
public:
    void begin(MTL::CommandBuffer* commands, const RenderPassDesc& desc) {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        for (std::size_t i = 0; i < desc.colorAttachments.size(); ++i) {
            const RenderPassColorAttachment& attachment = desc.colorAttachments[i];
            auto* target = static_cast<MetalTexture*>(attachment.texture);
            KUMO_ASSERT(target != nullptr);
            MTL::RenderPassColorAttachmentDescriptor* color =
                pass->colorAttachments()->object(static_cast<NS::UInteger>(i));
            color->setTexture(target->handle());
            color->setLoadAction(toMtl(attachment.loadOp));
            color->setStoreAction(toMtl(attachment.storeOp));
            color->setClearColor(
                MTL::ClearColor::Make(attachment.clearColor.r, attachment.clearColor.g,
                                      attachment.clearColor.b, attachment.clearColor.a));
            if (attachment.resolveTarget != nullptr) {
                auto* resolve = static_cast<MetalTexture*>(attachment.resolveTarget);
                color->setResolveTexture(resolve->handle());
                color->setStoreAction(attachment.storeOp == StoreOp::Store
                                          ? MTL::StoreActionStoreAndMultisampleResolve
                                          : MTL::StoreActionMultisampleResolve);
            }
        }
        if (desc.depthAttachment.texture != nullptr) {
            auto* depthTarget = static_cast<MetalTexture*>(desc.depthAttachment.texture);
            MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
            depth->setTexture(depthTarget->handle());
            depth->setLoadAction(toMtl(desc.depthAttachment.loadOp));
            depth->setStoreAction(toMtl(desc.depthAttachment.storeOp));
            depth->setClearDepth(desc.depthAttachment.clearDepth);
        }

        passDescriptor_ = NS::RetainPtr(pass);
        encoder_ = NS::RetainPtr(commands->renderCommandEncoder(pass));
        KUMO_ASSERT(encoder_);
    }

    void setPipeline(RenderPipeline& pipeline) override {
        auto& metalPipeline = static_cast<MetalRenderPipeline&>(pipeline);
        encoder_->setRenderPipelineState(metalPipeline.state());
        if (metalPipeline.depthStencil() != nullptr) {
            encoder_->setDepthStencilState(metalPipeline.depthStencil());
        }
        encoder_->setCullMode(metalPipeline.cullMode());
        encoder_->setFrontFacingWinding(metalPipeline.winding());
        primitive_ = metalPipeline.primitive();
    }

    void setBindGroup(std::uint32_t set, BindGroup& group) override {
        KUMO_ASSERT(set < kMaxBindGroups);
        static_cast<MetalBindGroup&>(group).apply(encoder_.get(), set);
    }

    void setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset) override {
        KUMO_ASSERT(slot < kMaxVertexBufferSlots);
        encoder_->setVertexBuffer(static_cast<MetalBuffer&>(buffer).handle(), offset,
                                  vertexBufferIndex(slot));
    }

    void setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset) override {
        indexBuffer_ = static_cast<MetalBuffer&>(buffer).handle();
        indexType_ = format == IndexFormat::Uint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
        indexOffset_ = offset;
    }

    void setPushConstants(ShaderStage stages, const void* data, std::uint32_t size) override {
        KUMO_ASSERT(size <= 128);
        if (hasFlag(stages, ShaderStage::Vertex)) {
            encoder_->setVertexBytes(data, size, kPushConstantBufferIndex);
        }
        if (hasFlag(stages, ShaderStage::Fragment)) {
            encoder_->setFragmentBytes(data, size, kPushConstantBufferIndex);
        }
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount,
              std::uint32_t firstVertex) override {
        encoder_->drawPrimitives(primitive_, static_cast<NS::UInteger>(firstVertex),
                                 static_cast<NS::UInteger>(vertexCount),
                                 static_cast<NS::UInteger>(instanceCount));
    }

    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount,
                     std::uint32_t firstIndex) override {
        KUMO_ASSERT(indexBuffer_ != nullptr);
        const std::uint64_t offset =
            indexOffset_ +
            static_cast<std::uint64_t>(firstIndex) * (indexType_ == MTL::IndexTypeUInt16 ? 2u : 4u);
        encoder_->drawIndexedPrimitives(primitive_, static_cast<NS::UInteger>(indexCount),
                                        indexType_, indexBuffer_, offset,
                                        static_cast<NS::UInteger>(instanceCount));
    }

    void end() override {
        if (encoder_) {
            encoder_->endEncoding();
        }
    }

    void* nativeEncoderHandle() override { return encoder_.get(); }
    void* nativePassDescriptorHandle() override { return passDescriptor_.get(); }

    void reset() {
        encoder_.reset();
        passDescriptor_.reset();
        indexBuffer_ = nullptr;
    }

private:
    NS::SharedPtr<MTL::RenderCommandEncoder> encoder_;
    NS::SharedPtr<MTL::RenderPassDescriptor> passDescriptor_;
    MTL::PrimitiveType primitive_ = MTL::PrimitiveTypeTriangle;
    MTL::Buffer* indexBuffer_ = nullptr;
    MTL::IndexType indexType_ = MTL::IndexTypeUInt16;
    std::uint64_t indexOffset_ = 0;
};

class MetalCommandEncoder final : public CommandEncoder {
public:
    MetalCommandEncoder(MTL::CommandQueue* queue, dispatch_semaphore_t frameSemaphore)
        : frameSemaphore_(frameSemaphore) {
        pool_ = NS::AutoreleasePool::alloc()->init();
        commands_ = NS::RetainPtr(queue->commandBuffer());
        KUMO_ASSERT(commands_);
    }

    ~MetalCommandEncoder() override {
        if (!submitted_) {
            dispatch_semaphore_signal(frameSemaphore_);
        }
        if (pool_ != nullptr) {
            pool_->release();
        }
    }

    RenderPassEncoder& beginRenderPass(const RenderPassDesc& desc) override {
        passEncoder_.reset();
        passEncoder_.begin(commands_.get(), desc);
        return passEncoder_;
    }

    void finishAndSubmit(Surface* presentTo) override {
        KUMO_ASSERT(!submitted_);
        auto* surface = static_cast<MetalSurface*>(presentTo);
        if (surface != nullptr && surface->currentDrawable() != nullptr) {
            commands_->presentDrawable(surface->currentDrawable());
        }
        dispatch_semaphore_t semaphore = frameSemaphore_;
        commands_->addCompletedHandler(
            [semaphore](MTL::CommandBuffer*) { dispatch_semaphore_signal(semaphore); });
        commands_->commit();
        submitted_ = true;
        if (surface != nullptr) {
            surface->releaseDrawable();
        }
        passEncoder_.reset();
        commands_.reset();
        pool_->release();
        pool_ = nullptr;
    }

    void* nativeCommandBufferHandle() override { return commands_.get(); }

private:
    NS::AutoreleasePool* pool_ = nullptr;
    NS::SharedPtr<MTL::CommandBuffer> commands_;
    MetalRenderPassEncoder passEncoder_;
    dispatch_semaphore_t frameSemaphore_;
    bool submitted_ = false;
};

class MetalQueue final : public Queue {
public:
    explicit MetalQueue(NS::SharedPtr<MTL::CommandQueue> queue)
        : queue_(std::move(queue)), frameSemaphore_(dispatch_semaphore_create(kFramesInFlight)) {}

    ~MetalQueue() override {
        // Drain so no completion handler can signal the semaphore after release.
        waitIdle();
        dispatch_release(frameSemaphore_);
    }

    Ptr<CommandEncoder> createCommandEncoder() override {
        dispatch_semaphore_wait(frameSemaphore_, DISPATCH_TIME_FOREVER);
        return std::make_shared<MetalCommandEncoder>(queue_.get(), frameSemaphore_);
    }

    void waitIdle() override {
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            dispatch_semaphore_wait(frameSemaphore_, DISPATCH_TIME_FOREVER);
        }
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            dispatch_semaphore_signal(frameSemaphore_);
        }
    }

    void writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                     std::uint64_t size) override {
        auto& metalBuffer = static_cast<MetalBuffer&>(buffer);
        KUMO_ASSERT(offset + size <= metalBuffer.size());
        std::memcpy(static_cast<char*>(metalBuffer.handle()->contents()) + offset, data, size);
    }

    void writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                      Extent2D size) override {
        auto& metalTexture = static_cast<MetalTexture&>(texture);
        metalTexture.handle()->replaceRegion(MTL::Region::Make2D(0, 0, size.width, size.height), 0,
                                             data, bytesPerRow);
    }

private:
    NS::SharedPtr<MTL::CommandQueue> queue_;
    dispatch_semaphore_t frameSemaphore_;
};

class MetalDevice final : public Device {
public:
    explicit MetalDevice(NS::SharedPtr<MTL::Device> device)
        : device_(std::move(device)), queue_(NS::TransferPtr(device_->newCommandQueue())) {}

    Ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        KUMO_ASSERT(desc.size > 0);
        NS::SharedPtr<MTL::Buffer> buffer =
            NS::TransferPtr(device_->newBuffer(desc.size, MTL::ResourceStorageModeShared));
        if (!buffer) {
            logError("createBuffer failed (size {})", desc.size);
            return nullptr;
        }
        return std::make_shared<MetalBuffer>(std::move(buffer), desc.size);
    }

    Ptr<Texture> createTexture(const TextureDesc& desc) override {
        KUMO_ASSERT(desc.size.width > 0 && desc.size.height > 0);
        KUMO_ASSERT(desc.sampleCount == 1); // MSAA arrives with M4.
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(desc.dimension == TextureDimension::Cube ? MTL::TextureTypeCube
                                                                            : MTL::TextureType2D);
        descriptor->setPixelFormat(toMtl(desc.format));
        descriptor->setWidth(desc.size.width);
        descriptor->setHeight(desc.size.height);
        descriptor->setMipmapLevelCount(desc.mipLevelCount);
        descriptor->setSampleCount(desc.sampleCount);
        descriptor->setStorageMode(MTL::StorageModeShared);

        MTL::TextureUsage usage = 0;
        if (hasFlag(desc.usage, TextureUsage::Sampled)) {
            usage |= MTL::TextureUsageShaderRead;
        }
        if (hasFlag(desc.usage, TextureUsage::RenderTarget)) {
            usage |= MTL::TextureUsageRenderTarget;
            // writeTexture uses replaceRegion, which needs CPU-accessible storage.
            if (!hasFlag(desc.usage, TextureUsage::CopyDst) &&
                !hasFlag(desc.usage, TextureUsage::CopySrc)) {
                descriptor->setStorageMode(MTL::StorageModePrivate);
            }
        }
        if (hasFlag(desc.usage, TextureUsage::Storage)) {
            usage |= MTL::TextureUsageShaderWrite;
        }
        descriptor->setUsage(usage);

        NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(device_->newTexture(descriptor));
        descriptor->release();
        if (!texture) {
            logError("createTexture failed ({}x{})", desc.size.width, desc.size.height);
            return nullptr;
        }
        return std::make_shared<MetalTexture>(std::move(texture), desc);
    }

    Ptr<Sampler> createSampler(const SamplerDesc& desc) override {
        MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
        descriptor->setMagFilter(toMtl(desc.magFilter));
        descriptor->setMinFilter(toMtl(desc.minFilter));
        descriptor->setMipFilter(desc.mipFilter == FilterMode::Nearest
                                     ? MTL::SamplerMipFilterNearest
                                     : MTL::SamplerMipFilterLinear);
        descriptor->setSAddressMode(toMtl(desc.addressModeU));
        descriptor->setTAddressMode(toMtl(desc.addressModeV));
        descriptor->setRAddressMode(toMtl(desc.addressModeW));

        NS::SharedPtr<MTL::SamplerState> sampler =
            NS::TransferPtr(device_->newSamplerState(descriptor));
        descriptor->release();
        if (!sampler) {
            logError("createSampler failed");
            return nullptr;
        }
        return std::make_shared<MetalSampler>(std::move(sampler));
    }

    Ptr<ShaderModule> createShaderModule(const ShaderModuleDesc& desc) override {
        KUMO_ASSERT(desc.language == ShaderSourceLanguage::MSL);
        KUMO_ASSERT(!desc.entryPoint.empty());
        AutoreleasePoolGuard pool;
        NS::Error* error = nullptr;
        NS::SharedPtr<MTL::Library> library =
            NS::TransferPtr(device_->newLibrary(nsString(desc.source), nullptr, &error));
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
        return std::make_shared<MetalShaderModule>(std::move(library), std::move(function));
    }

    Ptr<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDesc& desc) override {
        for (const BindGroupLayoutEntry& entry : desc.entries) {
            KUMO_ASSERT(entry.binding < kMaxBindingsPerSet);
        }
        return std::make_shared<MetalBindGroupLayout>(desc);
    }

    Ptr<BindGroup> createBindGroup(const BindGroupDesc& desc) override {
        auto* layout = static_cast<MetalBindGroupLayout*>(desc.layout.get());
        KUMO_ASSERT(layout != nullptr);
        return std::make_shared<MetalBindGroup>(desc, *layout);
    }

    Ptr<RenderPipeline> createRenderPipeline(const RenderPipelineDesc& desc) override {
        KUMO_ASSERT(desc.vertexBuffers.size() <= kMaxVertexBufferSlots);
        KUMO_ASSERT(desc.bindGroupLayouts.size() <= kMaxBindGroups);
        KUMO_ASSERT(desc.pushConstantSize <= 128);
        KUMO_ASSERT(desc.sampleCount == 1); // MSAA arrives with M4.
        if (!desc.vertexShader || !desc.fragmentShader) {
            logError("createRenderPipeline requires vertex and fragment shaders");
            return nullptr;
        }
        AutoreleasePoolGuard pool;

        MTL::RenderPipelineDescriptor* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
        descriptor->setVertexFunction(
            static_cast<MetalShaderModule*>(desc.vertexShader.get())->function());
        descriptor->setFragmentFunction(
            static_cast<MetalShaderModule*>(desc.fragmentShader.get())->function());

        if (!desc.vertexBuffers.empty()) {
            MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::vertexDescriptor();
            for (std::size_t slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
                const VertexBufferLayout& layout = desc.vertexBuffers[slot];
                const std::uint32_t bufferIndex =
                    vertexBufferIndex(static_cast<std::uint32_t>(slot));
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
            NS::TransferPtr(device_->newRenderPipelineState(descriptor, &error));
        descriptor->release();
        if (!state) {
            logError("createRenderPipeline failed: {}",
                     error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
            return nullptr;
        }

        NS::SharedPtr<MTL::DepthStencilState> depthStencil;
        if (desc.depthStencil.format != TextureFormat::Undefined) {
            MTL::DepthStencilDescriptor* depthDescriptor =
                MTL::DepthStencilDescriptor::alloc()->init();
            depthDescriptor->setDepthCompareFunction(toMtl(desc.depthStencil.depthCompare));
            depthDescriptor->setDepthWriteEnabled(desc.depthStencil.depthWriteEnabled);
            depthStencil = NS::TransferPtr(device_->newDepthStencilState(depthDescriptor));
            depthDescriptor->release();
        }

        return std::make_shared<MetalRenderPipeline>(std::move(state), std::move(depthStencil),
                                                     desc);
    }

    Ptr<Surface> createSurface(const SurfaceDesc& desc) override {
        KUMO_ASSERT(desc.nativeLayer != nullptr);
        return std::make_shared<MetalSurface>(static_cast<CA::MetalLayer*>(desc.nativeLayer),
                                              device_.get());
    }

    Queue& queue() override { return queue_; }

    NativeHandles nativeHandles() override { return {.device = device_.get()}; }

private:
    NS::SharedPtr<MTL::Device> device_;
    MetalQueue queue_;
};

} // namespace

Ptr<Device> createDevice(const DeviceDesc& desc) {
    (void)desc; // Metal validation is enabled via environment (MTL_DEBUG_LAYER).
    AutoreleasePoolGuard pool;
    NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        logError("no Metal device available");
        return nullptr;
    }
    logInfo("Metal device: {}", device->name()->utf8String());
    return std::make_shared<MetalDevice>(std::move(device));
}

} // namespace kumo::rhi::metal
