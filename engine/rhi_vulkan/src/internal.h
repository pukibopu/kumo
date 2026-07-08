#pragma once

#include <vulkan/vulkan.h>

#include <VkBootstrap.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wshadow"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "kumo/rhi_vulkan/rhi_vulkan.h"

#include "kumo/core/assert.h"
#include "kumo/core/log.h"
#include "kumo/rhi_vulkan/layout_tracker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace kumo::rhi::vulkan {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxColorAttachments = 4;

inline VkFormat toVk(TextureFormat format) {
    switch (format) {
    case TextureFormat::Undefined:
        return VK_FORMAT_UNDEFINED;
    case TextureFormat::RGBA8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::RGBA8UnormSrgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::BGRA8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::BGRA8UnormSrgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case TextureFormat::RGBA16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::RG16Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case TextureFormat::R32Float:
        return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::RGBA32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureFormat::Depth32Float:
        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

inline VkFormat toVk(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float32:
        return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float32x2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float32x3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float32x4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::Uint32:
        return VK_FORMAT_R32_UINT;
    case VertexFormat::Unorm8x4:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

inline VkCompareOp toVk(CompareFunction function) {
    switch (function) {
    case CompareFunction::Never:
        return VK_COMPARE_OP_NEVER;
    case CompareFunction::Less:
        return VK_COMPARE_OP_LESS;
    case CompareFunction::Equal:
        return VK_COMPARE_OP_EQUAL;
    case CompareFunction::LessEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareFunction::Greater:
        return VK_COMPARE_OP_GREATER;
    case CompareFunction::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case CompareFunction::GreaterEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareFunction::Always:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

inline VkFilter toVkFilter(FilterMode mode) {
    return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

inline VkSamplerAddressMode toVk(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirrorRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

inline VkAttachmentLoadOp toVk(LoadOp op) {
    switch (op) {
    case LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

inline VkAttachmentStoreOp toVk(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

inline VkPrimitiveTopology toVk(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

inline VkDescriptorType toVk(BindingType type) {
    switch (type) {
    case BindingType::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::Texture:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::StorageTexture:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case BindingType::Sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

inline VkShaderStageFlags toVkStages(ShaderStage stages) {
    VkShaderStageFlags flags = 0;
    if (hasFlag(stages, ShaderStage::Vertex)) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (hasFlag(stages, ShaderStage::Fragment)) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (hasFlag(stages, ShaderStage::Compute)) {
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return flags;
}

inline bool isDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth32Float;
}

inline VkSampleCountFlagBits toVkSamples(std::uint32_t count) {
    switch (count) {
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (hasFlag(usage, BufferUsage::Vertex)) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::Index)) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::Uniform)) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::Storage)) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (hasFlag(usage, BufferUsage::CopySrc)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasFlag(usage, BufferUsage::CopyDst)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

class VulkanBuffer final : public Buffer {
public:
    VulkanBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mapped,
                 std::uint64_t size)
        : allocator_(allocator), buffer_(buffer), allocation_(allocation), mapped_(mapped),
          size_(size) {}

    ~VulkanBuffer() override {
        if (buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }
    }

    std::uint64_t size() const override { return size_; }
    VkBuffer handle() const { return buffer_; }
    void* mapped() const { return mapped_; }

private:
    VmaAllocator allocator_;
    VkBuffer buffer_;
    VmaAllocation allocation_;
    void* mapped_;
    std::uint64_t size_;
};

class VulkanTexture final : public Texture {
public:
    // Owns both image and view.
    VulkanTexture(VkDevice device, VmaAllocator allocator, VkImage image, VmaAllocation allocation,
                  VkImageView view, const TextureDesc& desc)
        : device_(device), allocator_(allocator), image_(image), view_(view),
          allocation_(allocation), extent_(desc.size), format_(desc.format),
          mipLevels_(desc.mipLevelCount),
          arrayLayers_(desc.dimension == TextureDimension::Cube ? 6u : 1u),
          sampleCount_(desc.sampleCount), ownsImage_(true), ownsView_(true) {}

    // View over a parent image: owns only the view, retains the parent alive, and
    // keys layout tracking on the parent VkImage so transitions stay coherent.
    VulkanTexture(VkDevice device, Ptr<Texture> parent, VkImage parentImage, VkImageView view,
                  Extent2D extent, TextureFormat format)
        : device_(device), image_(parentImage), view_(view), extent_(extent), format_(format),
          ownsView_(true), parent_(std::move(parent)) {}

    // Transient wrapper around a swapchain image.
    VulkanTexture() = default;

    ~VulkanTexture() override {
        if (ownsView_ && view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view_, nullptr);
        }
        if (ownsImage_ && image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, image_, allocation_);
        }
    }

    void reset(VkImage image, VkImageView view, Extent2D extent, TextureFormat format) {
        image_ = image;
        view_ = view;
        extent_ = extent;
        format_ = format;
        ownsImage_ = false;
        ownsView_ = false;
    }

    Extent2D extent() const override { return extent_; }
    TextureFormat format() const override { return format_; }
    std::uint32_t sampleCount() const override { return sampleCount_; }
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    std::uint32_t mipLevels() const { return mipLevels_; }
    std::uint32_t arrayLayers() const { return arrayLayers_; }
    VkImageAspectFlags aspect() const {
        return isDepthFormat(format_) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    Extent2D extent_;
    TextureFormat format_ = TextureFormat::Undefined;
    std::uint32_t mipLevels_ = 1;
    std::uint32_t arrayLayers_ = 1;
    std::uint32_t sampleCount_ = 1;
    bool ownsImage_ = false;
    bool ownsView_ = false;
    Ptr<Texture> parent_;
};

class VulkanSampler final : public Sampler {
public:
    VulkanSampler(VkDevice device, VkSampler sampler) : device_(device), sampler_(sampler) {}
    ~VulkanSampler() override {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
        }
    }
    VkSampler handle() const { return sampler_; }

private:
    VkDevice device_;
    VkSampler sampler_;
};

class VulkanShaderModule final : public ShaderModule {
public:
    VulkanShaderModule(VkDevice device, VkShaderModule module, std::string entryPoint)
        : device_(device), module_(module), entryPoint_(std::move(entryPoint)) {}
    ~VulkanShaderModule() override {
        if (module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, module_, nullptr);
        }
    }
    VkShaderModule handle() const { return module_; }
    const std::string& entryPoint() const { return entryPoint_; }

private:
    VkDevice device_;
    VkShaderModule module_;
    std::string entryPoint_;
};

class VulkanBindGroupLayout final : public BindGroupLayout {
public:
    VulkanBindGroupLayout(VkDevice device, VkDescriptorSetLayout layout, BindGroupLayoutDesc desc)
        : device_(device), layout_(layout), desc_(std::move(desc)) {}
    ~VulkanBindGroupLayout() override {
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        }
    }
    VkDescriptorSetLayout handle() const { return layout_; }
    const BindGroupLayoutDesc& desc() const { return desc_; }

private:
    VkDevice device_;
    VkDescriptorSetLayout layout_;
    BindGroupLayoutDesc desc_;
};

class VulkanBindGroup final : public BindGroup {
public:
    VulkanBindGroup(VkDescriptorSet set, std::vector<VkImage> storageImages)
        : set_(set), storageImages_(std::move(storageImages)) {}
    VkDescriptorSet handle() const { return set_; }
    // Parent VkImages of storage-image entries; a compute pass transitions these
    // to GENERAL before dispatch and to SHADER_READ_ONLY when it ends.
    const std::vector<VkImage>& storageImages() const { return storageImages_; }

private:
    VkDescriptorSet set_; // owned by a device pool; never freed individually
    std::vector<VkImage> storageImages_;
};

class VulkanRenderPipeline final : public RenderPipeline {
public:
    VulkanRenderPipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout,
                         std::uint32_t sampleCount)
        : device_(device), pipeline_(pipeline), layout_(layout), sampleCount_(sampleCount) {}
    ~VulkanRenderPipeline() override {
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        }
    }
    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    std::uint32_t sampleCount() const { return sampleCount_; }

private:
    VkDevice device_;
    VkPipeline pipeline_;
    VkPipelineLayout layout_;
    std::uint32_t sampleCount_;
};

class VulkanComputePipeline final : public ComputePipeline {
public:
    VulkanComputePipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout)
        : device_(device), pipeline_(pipeline), layout_(layout) {}
    ~VulkanComputePipeline() override {
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        }
    }
    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

private:
    VkDevice device_;
    VkPipeline pipeline_;
    VkPipelineLayout layout_;
};

struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    bool pendingAcquire = false;
};

class VulkanQueue; // fwd

class VulkanSurface final : public Surface {
public:
    VulkanSurface(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  std::uint32_t graphicsFamily, VkSurfaceKHR surface, VulkanQueue* queue,
                  LayoutTracker* tracker)
        : instance_(instance), physical_(physical), device_(device),
          graphicsFamily_(graphicsFamily), surface_(surface), queue_(queue), tracker_(tracker) {}

    ~VulkanSurface() override;

    void configure(const SurfaceConfig& config) override;

    Texture* acquireNextTexture() override;

    std::uint32_t imageCount() const override { return static_cast<std::uint32_t>(images_.size()); }
    TextureFormat format() const override { return format_; }

    bool acquired() const { return acquired_; }
    void clearAcquired() { acquired_ = false; }
    void markNeedsRecreate() { needsRecreate_ = true; }
    VulkanTexture* currentTexture() { return &texture_; }
    std::uint32_t currentImageIndex() const { return currentImageIndex_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkSemaphore currentRenderFinishedSemaphore() const {
        return renderFinished_[currentImageIndex_];
    }

private:
    VkSemaphore imageAvailableSemaphore() const; // defined after VulkanQueue
    void markAcquireSignaled();                  // defined after VulkanQueue

    void forgetSwapchainImages();

    void destroySwapchain();

    void recreate();

    VkInstance instance_;
    VkPhysicalDevice physical_;
    VkDevice device_;
    std::uint32_t graphicsFamily_;
    VkSurfaceKHR surface_;
    VulkanQueue* queue_;
    LayoutTracker* tracker_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkSemaphore> retiredSemaphores_;
    VulkanTexture texture_;
    Extent2D extent_;
    TextureFormat format_ = TextureFormat::BGRA8Unorm;
    std::uint32_t currentImageIndex_ = 0;
    bool acquired_ = false;
    bool needsRecreate_ = false;
};

class VulkanQueue final : public Queue {
public:
    bool init(VkPhysicalDevice physical, VkDevice device, VkQueue queue, std::uint32_t family,
              VmaAllocator allocator, VkCommandPool uploadPool, LayoutTracker* tracker);

    void shutdown();

    Ptr<CommandEncoder> createCommandEncoder() override;

    void waitIdle() override;

    void writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                     std::uint64_t size) override;

    void writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                      Extent2D size) override;

    VkQueue handle() const { return queue_; }
    VkSemaphore currentImageAvailableSemaphore() const {
        KUMO_ASSERT(activeSlot_ != nullptr);
        return activeSlot_->imageAvailable;
    }
    void markAcquireSignaled() {
        KUMO_ASSERT(activeSlot_ != nullptr);
        activeSlot_->pendingAcquire = true;
    }

private:
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    LayoutTracker* tracker_ = nullptr;
    FrameSlot frames_[kFramesInFlight];
    std::uint32_t frameIndex_ = 0;
    FrameSlot* activeSlot_ = nullptr;
};

class VulkanDevice final : public Device {
public:
    VulkanDevice(vkb::Instance instance, vkb::PhysicalDevice physical, vkb::Device device,
                 VkQueue graphicsQueue, std::uint32_t graphicsFamily);

    ~VulkanDevice() override;

    bool ok() const { return ok_; }

    Ptr<Buffer> createBuffer(const BufferDesc& desc) override;

    Ptr<Texture> createTexture(const TextureDesc& desc) override;

    Ptr<Texture> createTextureView(const Ptr<Texture>& texture,
                                   const TextureViewDesc& desc) override;

    Ptr<Sampler> createSampler(const SamplerDesc& desc) override;

    Ptr<ShaderModule> createShaderModule(const ShaderModuleDesc& desc) override;

    Ptr<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDesc& desc) override;

    Ptr<BindGroup> createBindGroup(const BindGroupDesc& desc) override;

    Ptr<RenderPipeline> createRenderPipeline(const RenderPipelineDesc& desc) override;

    Ptr<ComputePipeline> createComputePipeline(const ComputePipelineDesc& desc) override;

    Ptr<Surface> createSurface(const SurfaceDesc& desc) override;

    Queue& queue() override { return queue_; }

    NativeHandles nativeHandles() override;

private:
    VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);

    void createDescriptorPool();

    vkb::Instance vkbInstance_;
    vkb::PhysicalDevice vkbPhysical_;
    vkb::Device vkbDevice_;
    VkInstance instance_;
    VkPhysicalDevice physical_;
    VkDevice device_;
    VkQueue graphicsQueue_;
    std::uint32_t graphicsFamily_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout emptySetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools_;
    LayoutTracker tracker_;
    VulkanQueue queue_;
    float maxSamplerAnisotropy_ = 1.0f;
    bool ok_ = false;
};

} // namespace kumo::rhi::vulkan
