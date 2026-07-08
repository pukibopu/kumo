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
#define VMA_IMPLEMENTATION
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

namespace {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxColorAttachments = 4;

VkFormat toVk(TextureFormat format) {
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

VkFormat toVk(VertexFormat format) {
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

VkCompareOp toVk(CompareFunction function) {
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

VkFilter toVkFilter(FilterMode mode) {
    return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode toVk(AddressMode mode) {
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

VkAttachmentLoadOp toVk(LoadOp op) {
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

VkAttachmentStoreOp toVk(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkPrimitiveTopology toVk(PrimitiveTopology topology) {
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

VkDescriptorType toVk(BindingType type) {
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

VkShaderStageFlags toVkStages(ShaderStage stages) {
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

bool isDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth32Float;
}

VkSampleCountFlagBits toVkSamples(std::uint32_t count) {
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

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
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

struct BarrierMasks {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

BarrierMasks masksFor(ImageLayoutState state) {
    switch (state) {
    case ImageLayoutState::Undefined:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
    case ImageLayoutState::ColorAttachment:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case ImageLayoutState::DepthAttachment:
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ImageLayoutState::ShaderReadOnly:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT};
    case ImageLayoutState::TransferSrc:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT};
    case ImageLayoutState::TransferDst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ImageLayoutState::General:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ImageLayoutState::PresentSrc:
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                0};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
}

// Attachment-write scope for a same-layout write-after-write barrier on a
// persistent attachment reused across frames (no layout change to react to).
BarrierMasks writeMasksFor(ImageLayoutState state) {
    if (state == ImageLayoutState::DepthAttachment) {
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    }
    return masksFor(state);
}

VkImageMemoryBarrier2 makeBarrier(VkImage image, VkImageAspectFlags aspect, const BarrierMasks& src,
                                  const BarrierMasks& dst) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src.stage;
    barrier.srcAccessMask = src.access;
    barrier.dstStageMask = dst.stage;
    barrier.dstAccessMask = dst.access;
    barrier.oldLayout = src.layout;
    barrier.newLayout = dst.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    // Layouts are tracked per-image, so barriers cover all mips/layers together.
    // Compute prefilter passes write mips sequentially, keeping this coherent.
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return barrier;
}

void recordBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                   LayoutTransition transition) {
    const VkImageMemoryBarrier2 barrier =
        makeBarrier(image, aspect, masksFor(transition.oldLayout), masksFor(transition.newLayout));
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
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

class VulkanQueue; // fwd

class VulkanSurface final : public Surface {
public:
    VulkanSurface(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  std::uint32_t graphicsFamily, VkSurfaceKHR surface, VulkanQueue* queue,
                  LayoutTracker* tracker)
        : instance_(instance), physical_(physical), device_(device),
          graphicsFamily_(graphicsFamily), surface_(surface), queue_(queue), tracker_(tracker) {}

    ~VulkanSurface() override {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
    }

    void configure(const SurfaceConfig& config) override {
        vkDeviceWaitIdle(device_);
        format_ = config.format;
        extent_ = config.size;
        recreate();
    }

    Texture* acquireNextTexture() override {
        if (needsRecreate_) {
            vkDeviceWaitIdle(device_);
            recreate();
        }
        if (swapchain_ == VK_NULL_HANDLE) {
            return nullptr;
        }
        const VkSemaphore imageAvailable = imageAvailableSemaphore();
        std::uint32_t index = 0;
        const VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                      imageAvailable, VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            needsRecreate_ = true;
            return nullptr;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            logError("vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
            return nullptr;
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            needsRecreate_ = true;
        }
        currentImageIndex_ = index;
        acquired_ = true;
        markAcquireSignaled();
        texture_.reset(images_[index], imageViews_[index], extent_, format_);
        return &texture_;
    }

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

    void forgetSwapchainImages() {
        if (tracker_ != nullptr) {
            for (VkImage image : images_) {
                tracker_->forget(reinterpret_cast<std::uint64_t>(image));
            }
        }
    }

    void destroySwapchain() {
        forgetSwapchainImages();
        for (VkImageView view : imageViews_) {
            vkDestroyImageView(device_, view, nullptr);
        }
        imageViews_.clear();
        for (VkSemaphore semaphore : renderFinished_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        renderFinished_.clear();
        for (VkSemaphore semaphore : retiredSemaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        retiredSemaphores_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        images_.clear();
    }

    void recreate() {
        if (extent_.width == 0 || extent_.height == 0) {
            destroySwapchain();
            needsRecreate_ = true;
            return;
        }
        const VkSwapchainKHR oldSwapchain = swapchain_;
        std::vector<VkImageView> oldViews = imageViews_;
        std::vector<VkSemaphore> oldRenderFinished = renderFinished_;

        vkb::SwapchainBuilder builder{physical_, device_, surface_,
                                      static_cast<std::uint32_t>(graphicsFamily_),
                                      static_cast<std::uint32_t>(graphicsFamily_)};
        auto swap_ret = builder
                            .set_desired_format(VkSurfaceFormatKHR{
                                toVk(format_), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                            .set_desired_extent(extent_.width, extent_.height)
                            .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                            .set_old_swapchain(oldSwapchain)
                            .build();
        if (!swap_ret) {
            logError("swapchain build failed: {}", swap_ret.error().message());
            needsRecreate_ = true;
            return;
        }
        vkb::Swapchain vkbSwapchain = swap_ret.value();

        // Semaphores retired by the previous recreate are now idle; destroy them.
        for (VkSemaphore semaphore : retiredSemaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        retiredSemaphores_.clear();
        for (VkImageView view : oldViews) {
            vkDestroyImageView(device_, view, nullptr);
        }
        // The presentation engine may still hold renderFinished after SUBOPTIMAL;
        // waitIdle does not cover it, so retire rather than destroy immediately.
        retiredSemaphores_ = std::move(oldRenderFinished);
        forgetSwapchainImages();
        if (oldSwapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
        }

        swapchain_ = vkbSwapchain.swapchain;
        extent_ = {vkbSwapchain.extent.width, vkbSwapchain.extent.height};
        images_ = vkbSwapchain.get_images().value();
        imageViews_ = vkbSwapchain.get_image_views().value();

        renderFinished_.resize(images_.size());
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore& semaphore : renderFinished_) {
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        }
        needsRecreate_ = false;
    }

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

struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    bool pendingAcquire = false;
};

class VulkanRenderPassEncoder final : public RenderPassEncoder {
public:
    void begin(VkCommandBuffer cmd, LayoutTracker& tracker, const RenderPassDesc& desc) {
        cmd_ = cmd;
        open_ = true;

        KUMO_ASSERT(desc.colorAttachments.size() <= kMaxColorAttachments);
        std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colorInfos{};
        // Each color attachment may add its own barrier plus one for a resolve
        // target; the depth attachment adds one more.
        std::array<VkImageMemoryBarrier2, kMaxColorAttachments * 2 + 1> barriers{};
        std::uint32_t colorCount = 0;
        std::uint32_t barrierCount = 0;
        Extent2D renderExtent{};

        for (const RenderPassColorAttachment& attachment : desc.colorAttachments) {
            auto* target = static_cast<VulkanTexture*>(attachment.texture);
            KUMO_ASSERT(target != nullptr);
            renderExtent = target->extent();
            if (colorCount == 0) {
                passSampleCount_ = target->sampleCount();
            }

            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker.request(id, ImageLayoutState::ColorAttachment)) {
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_COLOR_BIT,
                                masksFor(transition->oldLayout), masksFor(transition->newLayout));
            } else {
                // No layout change, but a persistent attachment reused next frame
                // still needs a write-after-write barrier.
                const BarrierMasks waw = writeMasksFor(ImageLayoutState::ColorAttachment);
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_COLOR_BIT, waw, waw);
            }

            VkRenderingAttachmentInfo& info = colorInfos[colorCount++];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = target->view();
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = toVk(attachment.loadOp);
            info.storeOp = toVk(attachment.storeOp);
            info.clearValue.color = {{attachment.clearColor.r, attachment.clearColor.g,
                                      attachment.clearColor.b, attachment.clearColor.a}};

            if (attachment.resolveTarget != nullptr) {
                auto* resolve = static_cast<VulkanTexture*>(attachment.resolveTarget);
                const auto resolveId = reinterpret_cast<std::uint64_t>(resolve->image());
                if (auto transition =
                        tracker.request(resolveId, ImageLayoutState::ColorAttachment)) {
                    barriers[barrierCount++] = makeBarrier(
                        resolve->image(), VK_IMAGE_ASPECT_COLOR_BIT,
                        masksFor(transition->oldLayout), masksFor(transition->newLayout));
                } else {
                    const BarrierMasks waw = writeMasksFor(ImageLayoutState::ColorAttachment);
                    barriers[barrierCount++] =
                        makeBarrier(resolve->image(), VK_IMAGE_ASPECT_COLOR_BIT, waw, waw);
                }
                info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                info.resolveImageView = resolve->view();
                info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }

        VkRenderingAttachmentInfo depthInfo{};
        bool hasDepth = false;
        if (desc.depthAttachment.texture != nullptr) {
            auto* target = static_cast<VulkanTexture*>(desc.depthAttachment.texture);
            renderExtent = target->extent();
            if (colorCount == 0) {
                passSampleCount_ = target->sampleCount();
            }
            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker.request(id, ImageLayoutState::DepthAttachment)) {
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                masksFor(transition->oldLayout), masksFor(transition->newLayout));
            } else {
                const BarrierMasks waw = writeMasksFor(ImageLayoutState::DepthAttachment);
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_DEPTH_BIT, waw, waw);
            }
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = target->view();
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = toVk(desc.depthAttachment.loadOp);
            depthInfo.storeOp = toVk(desc.depthAttachment.storeOp);
            depthInfo.clearValue.depthStencil = {desc.depthAttachment.clearDepth, 0};
            hasDepth = true;
        }

        if (barrierCount > 0) {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = barrierCount;
            dependency.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(cmd_, &dependency);
        }

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {renderExtent.width, renderExtent.height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = colorCount;
        rendering.pColorAttachments = colorInfos.data();
        rendering.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        vkCmdBeginRendering(cmd_, &rendering);

        // Negative-height viewport flips Y so the same GLSL clip space renders
        // identically to Metal.
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(renderExtent.height);
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = -static_cast<float>(renderExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd_, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, {renderExtent.width, renderExtent.height}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void setPipeline(RenderPipeline& pipeline) override {
        auto& vkPipeline = static_cast<VulkanRenderPipeline&>(pipeline);
        KUMO_ASSERT(vkPipeline.sampleCount() == passSampleCount_);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline.handle());
        currentLayout_ = vkPipeline.layout();
    }

    void setBindGroup(std::uint32_t set, BindGroup& group) override {
        VkDescriptorSet descriptorSet = static_cast<VulkanBindGroup&>(group).handle();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, currentLayout_, set, 1,
                                &descriptorSet, 0, nullptr);
    }

    void setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset) override {
        VkBuffer handle = static_cast<VulkanBuffer&>(buffer).handle();
        VkDeviceSize vkOffset = offset;
        vkCmdBindVertexBuffers(cmd_, slot, 1, &handle, &vkOffset);
    }

    void setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset) override {
        vkCmdBindIndexBuffer(cmd_, static_cast<VulkanBuffer&>(buffer).handle(), offset,
                             format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16
                                                           : VK_INDEX_TYPE_UINT32);
    }

    void setPushConstants(ShaderStage stages, const void* data, std::uint32_t size) override {
        vkCmdPushConstants(cmd_, currentLayout_, toVkStages(stages), 0, size, data);
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount,
              std::uint32_t firstVertex) override {
        vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, 0);
    }

    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount,
                     std::uint32_t firstIndex) override {
        vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, 0, 0);
    }

    void end() override {
        if (cmd_ != VK_NULL_HANDLE && open_) {
            vkCmdEndRendering(cmd_);
        }
        open_ = false;
    }

    void* nativeEncoderHandle() override { return cmd_; }
    void* nativePassDescriptorHandle() override { return nullptr; }

    void reset() {
        cmd_ = VK_NULL_HANDLE;
        open_ = false;
    }
    bool open() const { return open_; }

private:
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;
    std::uint32_t passSampleCount_ = 1;
    bool open_ = false;
};

class VulkanComputePassEncoder final : public ComputePassEncoder {
public:
    void begin(VkCommandBuffer cmd, LayoutTracker& tracker) {
        cmd_ = cmd;
        tracker_ = &tracker;
        open_ = true;
        dispatched_ = false;
        touched_.clear();
    }

    void setPipeline(ComputePipeline& pipeline) override {
        auto& vkPipeline = static_cast<VulkanComputePipeline&>(pipeline);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, vkPipeline.handle());
        currentLayout_ = vkPipeline.layout();
    }

    void setBindGroup(std::uint32_t set, BindGroup& group) override {
        auto& vkGroup = static_cast<VulkanBindGroup&>(group);
        for (VkImage image : vkGroup.storageImages()) {
            const auto id = reinterpret_cast<std::uint64_t>(image);
            if (auto transition = tracker_->request(id, ImageLayoutState::General)) {
                recordBarrier(cmd_, image, VK_IMAGE_ASPECT_COLOR_BIT, *transition);
            }
            if (std::find(touched_.begin(), touched_.end(), image) == touched_.end()) {
                touched_.push_back(image);
            }
        }
        VkDescriptorSet descriptorSet = vkGroup.handle();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, currentLayout_, set, 1,
                                &descriptorSet, 0, nullptr);
    }

    void setPushConstants(const void* data, std::uint32_t size) override {
        vkCmdPushConstants(cmd_, currentLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
    }

    void dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override {
        if (dispatched_) {
            // Coarse inter-dispatch barrier, per the implicit sync model: successive
            // dispatches on the same storage image have no auto RAW/WAW sync here.
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd_, &dependency);
        }
        vkCmdDispatch(cmd_, groupsX, groupsY, groupsZ);
        dispatched_ = true;
    }

    void end() override {
        if (cmd_ != VK_NULL_HANDLE && open_) {
            // Storage writes are complete; make the images samplable by later passes.
            for (VkImage image : touched_) {
                const auto id = reinterpret_cast<std::uint64_t>(image);
                if (auto transition = tracker_->request(id, ImageLayoutState::ShaderReadOnly)) {
                    recordBarrier(cmd_, image, VK_IMAGE_ASPECT_COLOR_BIT, *transition);
                }
            }
        }
        open_ = false;
    }

    void reset() {
        cmd_ = VK_NULL_HANDLE;
        open_ = false;
        dispatched_ = false;
        touched_.clear();
    }
    bool open() const { return open_; }

private:
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    LayoutTracker* tracker_ = nullptr;
    VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;
    std::vector<VkImage> touched_;
    bool dispatched_ = false;
    bool open_ = false;
};

class VulkanCommandEncoder final : public CommandEncoder {
public:
    VulkanCommandEncoder(VkPhysicalDevice physical, VkQueue queue, FrameSlot* slot,
                         LayoutTracker* tracker)
        : physical_(physical), queue_(queue), slot_(slot), tracker_(tracker) {}

    ~VulkanCommandEncoder() override {
        if (submitted_) {
            return;
        }
        // Drain the frame slot so its fence is signaled for the next reuse.
        if (passEncoder_.open()) {
            passEncoder_.end();
        }
        if (computePassEncoder_.open()) {
            computePassEncoder_.end();
        }
        vkEndCommandBuffer(slot_->cmd);

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = slot_->cmd;

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = slot_->imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        if (slot_->pendingAcquire) {
            // acquireNextTexture signaled this slot's semaphore but nothing
            // consumed it; the drain must wait so the signal is not left dangling.
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &waitInfo;
            slot_->pendingAcquire = false;
        }
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        vkQueueSubmit2(queue_, 1, &submit, slot_->inFlight);
    }

    RenderPassEncoder& beginRenderPass(const RenderPassDesc& desc) override {
        passEncoder_.reset();
        passEncoder_.begin(slot_->cmd, *tracker_, desc);
        return passEncoder_;
    }

    ComputePassEncoder& beginComputePass() override {
        computePassEncoder_.reset();
        computePassEncoder_.begin(slot_->cmd, *tracker_);
        return computePassEncoder_;
    }

    void generateMipmaps(Texture& texture) override {
        KUMO_ASSERT(!passEncoder_.open() && !computePassEncoder_.open());
        auto& tex = static_cast<VulkanTexture&>(texture);
        const std::uint32_t mipCount = tex.mipLevels();
        if (mipCount <= 1) {
            return;
        }
        // Linear-filter blits need a filterable format; HDR mip chains should use
        // RGBA16Float (RGBA32Float is not filterable on most GPUs).
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(physical_, toVk(tex.format()), &formatProps);
        if ((formatProps.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
            logError("generateMipmaps: format is not linear-filterable "
                     "(HDR mip chains should use RGBA16Float)");
            return;
        }
        const std::uint32_t layerCount = tex.arrayLayers();
        const VkImage image = tex.image();
        VkCommandBuffer cmd = slot_->cmd;
        const auto id = reinterpret_cast<std::uint64_t>(image);
        const VkImageLayout srcLayout = masksFor(tracker_->current(id)).layout;

        auto emit = [&](VkImageMemoryBarrier2& b) {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

        VkImageMemoryBarrier2 toSrc{};
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = srcLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.subresourceRange = {aspect, 0, 1, 0, layerCount};
        emit(toSrc);

        std::int32_t mipW = static_cast<std::int32_t>(tex.extent().width);
        std::int32_t mipH = static_cast<std::int32_t>(tex.extent().height);
        for (std::uint32_t i = 1; i < mipCount; ++i) {
            const std::int32_t dstW = std::max(1, mipW / 2);
            const std::int32_t dstH = std::max(1, mipH / 2);

            VkImageMemoryBarrier2 toDst{};
            toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            toDst.srcAccessMask = 0;
            toDst.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.subresourceRange = {aspect, i, 1, 0, layerCount};
            emit(toDst);

            VkImageBlit2 blit{};
            blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            blit.srcSubresource = {aspect, i - 1, 0, layerCount};
            blit.srcOffsets[1] = {mipW, mipH, 1};
            blit.dstSubresource = {aspect, i, 0, layerCount};
            blit.dstOffsets[1] = {dstW, dstH, 1};
            VkBlitImageInfo2 blitInfo{};
            blitInfo.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blitInfo.srcImage = image;
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.dstImage = image;
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.regionCount = 1;
            blitInfo.pRegions = &blit;
            blitInfo.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blitInfo);

            VkImageMemoryBarrier2 dstToSrc{};
            dstToSrc.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            dstToSrc.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            dstToSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            dstToSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            dstToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dstToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            dstToSrc.subresourceRange = {aspect, i, 1, 0, layerCount};
            emit(dstToSrc);

            mipW = dstW;
            mipH = dstH;
        }

        VkImageMemoryBarrier2 toRead{};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toRead.dstStageMask =
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.subresourceRange = {aspect, 0, mipCount, 0, layerCount};
        emit(toRead);
        tracker_->set(id, ImageLayoutState::ShaderReadOnly);
    }

    void finishAndSubmit(Surface* presentTo) override {
        KUMO_ASSERT(!submitted_);
        auto* surface = static_cast<VulkanSurface*>(presentTo);
        const bool present = surface != nullptr && surface->acquired();

        VkSemaphore renderFinished = VK_NULL_HANDLE;
        if (present) {
            // The wait below consumes the acquire semaphore signaled for this slot.
            slot_->pendingAcquire = false;
            VulkanTexture* target = surface->currentTexture();
            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker_->request(id, ImageLayoutState::PresentSrc)) {
                recordBarrier(slot_->cmd, target->image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
            }
            renderFinished = surface->currentRenderFinishedSemaphore();
        }

        vkEndCommandBuffer(slot_->cmd);

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = slot_->cmd;

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = slot_->imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = renderFinished;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = present ? 1u : 0u;
        submit.pWaitSemaphoreInfos = present ? &waitInfo : nullptr;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = present ? 1u : 0u;
        submit.pSignalSemaphoreInfos = present ? &signalInfo : nullptr;
        vkQueueSubmit2(queue_, 1, &submit, slot_->inFlight);
        submitted_ = true;

        if (present) {
            const std::uint32_t imageIndex = surface->currentImageIndex();
            const VkSwapchainKHR swapchain = surface->swapchain();
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &renderFinished;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchain;
            presentInfo.pImageIndices = &imageIndex;
            const VkResult result = vkQueuePresentKHR(queue_, &presentInfo);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                surface->markNeedsRecreate();
            } else if (result != VK_SUCCESS) {
                logError("vkQueuePresentKHR failed ({})", static_cast<int>(result));
            }
            surface->clearAcquired();
        }

        passEncoder_.reset();
        computePassEncoder_.reset();
    }

    void* nativeCommandBufferHandle() override { return slot_->cmd; }

private:
    VkPhysicalDevice physical_;
    VkQueue queue_;
    FrameSlot* slot_;
    LayoutTracker* tracker_;
    VulkanRenderPassEncoder passEncoder_;
    VulkanComputePassEncoder computePassEncoder_;
    bool submitted_ = false;
};

class VulkanQueue final : public Queue {
public:
    bool init(VkPhysicalDevice physical, VkDevice device, VkQueue queue, std::uint32_t family,
              VmaAllocator allocator, VkCommandPool uploadPool, LayoutTracker* tracker) {
        physical_ = physical;
        device_ = device;
        queue_ = queue;
        allocator_ = allocator;
        uploadPool_ = uploadPool;
        tracker_ = tracker;

        for (FrameSlot& slot : frames_) {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = family;
            if (vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.pool) != VK_SUCCESS) {
                logError("vkCreateCommandPool failed");
                return false;
            }
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = slot.pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &allocInfo, &slot.cmd) != VK_SUCCESS) {
                logError("vkAllocateCommandBuffers failed");
                return false;
            }
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device_, &fenceInfo, nullptr, &slot.inFlight) != VK_SUCCESS) {
                logError("vkCreateFence failed");
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.imageAvailable) !=
                VK_SUCCESS) {
                logError("vkCreateSemaphore failed");
                return false;
            }
        }
        return true;
    }

    void shutdown() {
        for (FrameSlot& slot : frames_) {
            if (slot.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
            }
            if (slot.inFlight != VK_NULL_HANDLE) {
                vkDestroyFence(device_, slot.inFlight, nullptr);
            }
            if (slot.pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, slot.pool, nullptr);
            }
            slot = {};
        }
    }

    Ptr<CommandEncoder> createCommandEncoder() override {
        FrameSlot& slot = frames_[frameIndex_];
        vkWaitForFences(device_, 1, &slot.inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &slot.inFlight);
        vkResetCommandPool(device_, slot.pool, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(slot.cmd, &beginInfo);

        activeSlot_ = &slot;
        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
        return std::make_shared<VulkanCommandEncoder>(physical_, queue_, &slot, tracker_);
    }

    void waitIdle() override { vkDeviceWaitIdle(device_); }

    void writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                     std::uint64_t size) override {
        auto& vkBuffer = static_cast<VulkanBuffer&>(buffer);
        KUMO_ASSERT(offset + size <= vkBuffer.size());
        KUMO_ASSERT(vkBuffer.mapped() != nullptr);
        std::memcpy(static_cast<char*>(vkBuffer.mapped()) + offset, data, size);
    }

    void writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                      Extent2D size) override {
        auto& vkTexture = static_cast<VulkanTexture&>(texture);
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(bytesPerRow) * size.height;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = dataSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(allocator_, &bufferInfo, &allocCreate, &staging, &stagingAlloc,
                            &stagingInfo) != VK_SUCCESS) {
            logError("writeTexture staging allocation failed");
            return;
        }
        std::memcpy(stagingInfo.pMappedData, data, dataSize);

        VkCommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAlloc.commandPool = uploadPool_;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cmdAlloc, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        const auto id = reinterpret_cast<std::uint64_t>(vkTexture.image());
        if (auto transition = tracker_->request(id, ImageLayoutState::TransferDst)) {
            recordBarrier(cmd, vkTexture.image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
        }

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource = {static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0,
                                 1};
        copy.imageExtent = {size.width, size.height, 1};
        vkCmdCopyBufferToImage(cmd, staging, vkTexture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        if (auto transition = tracker_->request(id, ImageLayoutState::ShaderReadOnly)) {
            recordBarrier(cmd, vkTexture.image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
        }

        vkEndCommandBuffer(cmd);

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);

        vkFreeCommandBuffers(device_, uploadPool_, 1, &cmd);
        vmaDestroyBuffer(allocator_, staging, stagingAlloc);
    }

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

VkSemaphore VulkanSurface::imageAvailableSemaphore() const {
    return queue_->currentImageAvailableSemaphore();
}

void VulkanSurface::markAcquireSignaled() {
    queue_->markAcquireSignaled();
}

class VulkanDevice final : public Device {
public:
    VulkanDevice(vkb::Instance instance, vkb::PhysicalDevice physical, vkb::Device device,
                 VkQueue graphicsQueue, std::uint32_t graphicsFamily)
        : vkbInstance_(instance), vkbPhysical_(physical), vkbDevice_(device),
          instance_(instance.instance), physical_(physical.physical_device), device_(device.device),
          graphicsQueue_(graphicsQueue), graphicsFamily_(graphicsFamily) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);
        maxSamplerAnisotropy_ = props.limits.maxSamplerAnisotropy;

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = physical_;
        allocatorInfo.device = device_;
        allocatorInfo.instance = instance_;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &allocator_) != VK_SUCCESS) {
            logError("vmaCreateAllocator failed");
            return;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = graphicsFamily_;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &uploadPool_) != VK_SUCCESS) {
            logError("vkCreateCommandPool (upload) failed");
            return;
        }

        VkDescriptorSetLayoutCreateInfo emptyInfo{};
        emptyInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (vkCreateDescriptorSetLayout(device_, &emptyInfo, nullptr, &emptySetLayout_) !=
            VK_SUCCESS) {
            logError("vkCreateDescriptorSetLayout (empty) failed");
            return;
        }

        ok_ = queue_.init(physical_, device_, graphicsQueue_, graphicsFamily_, allocator_,
                          uploadPool_, &tracker_);
    }

    ~VulkanDevice() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        queue_.shutdown();
        for (VkDescriptorPool pool : descriptorPools_) {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }
        if (emptySetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, emptySetLayout_, nullptr);
        }
        if (uploadPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, uploadPool_, nullptr);
        }
        if (allocator_ != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator_);
        }
        vkb::destroy_device(vkbDevice_);
        vkb::destroy_instance(vkbInstance_);
    }

    bool ok() const { return ok_; }

    Ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        KUMO_ASSERT(desc.size > 0);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = desc.size;
        bufferInfo.usage = toVkBufferUsage(desc.usage);

        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreate.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocCreate.requiredFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator_, &bufferInfo, &allocCreate, &buffer, &allocation, &info) !=
            VK_SUCCESS) {
            logError("createBuffer failed (size {})", desc.size);
            return nullptr;
        }
        return std::make_shared<VulkanBuffer>(allocator_, buffer, allocation, info.pMappedData,
                                              desc.size);
    }

    Ptr<Texture> createTexture(const TextureDesc& desc) override {
        KUMO_ASSERT(desc.size.width > 0 && desc.size.height > 0);
        KUMO_ASSERT(desc.sampleCount == 1 || desc.sampleCount == 4);

        VkImageUsageFlags usage = 0;
        if (hasFlag(desc.usage, TextureUsage::Sampled)) {
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        if (hasFlag(desc.usage, TextureUsage::RenderTarget)) {
            usage |= isDepthFormat(desc.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (hasFlag(desc.usage, TextureUsage::Storage)) {
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        if (hasFlag(desc.usage, TextureUsage::CopySrc)) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        if (hasFlag(desc.usage, TextureUsage::CopyDst)) {
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = toVk(desc.format);
        imageInfo.extent = {desc.size.width, desc.size.height, 1};
        imageInfo.mipLevels = desc.mipLevelCount;
        imageInfo.arrayLayers = desc.dimension == TextureDimension::Cube ? 6 : 1;
        imageInfo.samples = toVkSamples(desc.sampleCount);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (desc.dimension == TextureDimension::Cube) {
            imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreate.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        if (vmaCreateImage(allocator_, &imageInfo, &allocCreate, &image, &allocation, nullptr) !=
            VK_SUCCESS) {
            logError("createTexture failed ({}x{})", desc.size.width, desc.size.height);
            return nullptr;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = desc.dimension == TextureDimension::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                     : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = toVk(desc.format);
        viewInfo.subresourceRange = {
            static_cast<VkImageAspectFlags>(isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                       : VK_IMAGE_ASPECT_COLOR_BIT),
            0, desc.mipLevelCount, 0, imageInfo.arrayLayers};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            logError("createTexture: image view failed");
            vmaDestroyImage(allocator_, image, allocation);
            return nullptr;
        }
        return std::make_shared<VulkanTexture>(device_, allocator_, image, allocation, view, desc);
    }

    Ptr<Texture> createTextureView(const Ptr<Texture>& texture,
                                   const TextureViewDesc& desc) override {
        KUMO_ASSERT(texture != nullptr);
        auto* parent = static_cast<VulkanTexture*>(texture.get());
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = parent->image();
        viewInfo.viewType = desc.dimension == TextureDimension::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                     : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = toVk(parent->format());
        viewInfo.subresourceRange = {parent->aspect(), desc.baseMipLevel, desc.mipLevelCount,
                                     desc.baseArrayLayer, desc.arrayLayerCount};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            logError("createTextureView failed");
            return nullptr;
        }
        const Extent2D base = parent->extent();
        const Extent2D extent{std::max(1u, base.width >> desc.baseMipLevel),
                              std::max(1u, base.height >> desc.baseMipLevel)};
        return std::make_shared<VulkanTexture>(device_, texture, parent->image(), view, extent,
                                               parent->format());
    }

    Ptr<Sampler> createSampler(const SamplerDesc& desc) override {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = toVkFilter(desc.magFilter);
        samplerInfo.minFilter = toVkFilter(desc.minFilter);
        samplerInfo.mipmapMode = desc.mipFilter == FilterMode::Nearest
                                     ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                     : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = toVk(desc.addressModeU);
        samplerInfo.addressModeV = toVk(desc.addressModeV);
        samplerInfo.addressModeW = toVk(desc.addressModeW);
        samplerInfo.maxLod = desc.lodMaxClamp;
        if (desc.maxAnisotropy > 1) {
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy =
                std::min(static_cast<float>(desc.maxAnisotropy), maxSamplerAnisotropy_);
        }
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
            logError("createSampler failed");
            return nullptr;
        }
        return std::make_shared<VulkanSampler>(device_, sampler);
    }

    Ptr<ShaderModule> createShaderModule(const ShaderModuleDesc& desc) override {
        KUMO_ASSERT(desc.language == ShaderSourceLanguage::SPIRV);
        KUMO_ASSERT(!desc.spirv.empty());
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = desc.spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = desc.spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            logError("createShaderModule failed");
            return nullptr;
        }
        return std::make_shared<VulkanShaderModule>(
            device_, module, desc.entryPoint.empty() ? "main" : desc.entryPoint);
    }

    Ptr<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDesc& desc) override {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(desc.entries.size());
        for (const BindGroupLayoutEntry& entry : desc.entries) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = entry.binding;
            binding.descriptorType = toVk(entry.type);
            binding.descriptorCount = 1;
            binding.stageFlags = toVkStages(entry.visibility);
            bindings.push_back(binding);
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
            logError("createBindGroupLayout failed");
            return nullptr;
        }
        return std::make_shared<VulkanBindGroupLayout>(device_, layout, desc);
    }

    Ptr<BindGroup> createBindGroup(const BindGroupDesc& desc) override {
        auto* layout = static_cast<VulkanBindGroupLayout*>(desc.layout.get());
        KUMO_ASSERT(layout != nullptr);
        const VkDescriptorSet set = allocateDescriptorSet(layout->handle());
        if (set == VK_NULL_HANDLE) {
            logError("createBindGroup: descriptor set allocation failed");
            return nullptr;
        }

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> bufferInfos(desc.entries.size());
        std::vector<VkDescriptorImageInfo> imageInfos(desc.entries.size());
        std::vector<VkImage> storageImages;
        writes.reserve(desc.entries.size());

        for (std::size_t i = 0; i < desc.entries.size(); ++i) {
            const BindGroupEntry& entry = desc.entries[i];
            const BindGroupLayoutEntry* match = nullptr;
            for (const BindGroupLayoutEntry& layoutEntry : layout->desc().entries) {
                if (layoutEntry.binding == entry.binding) {
                    match = &layoutEntry;
                    break;
                }
            }
            KUMO_ASSERT(match != nullptr);
            if (match == nullptr) {
                continue;
            }

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = entry.binding;
            write.descriptorCount = 1;
            write.descriptorType = toVk(match->type);

            switch (match->type) {
            case BindingType::UniformBuffer:
            case BindingType::StorageBuffer: {
                auto* buffer = static_cast<VulkanBuffer*>(entry.buffer.get());
                KUMO_ASSERT(buffer != nullptr);
                bufferInfos[i] = {buffer->handle(), entry.bufferOffset, VK_WHOLE_SIZE};
                write.pBufferInfo = &bufferInfos[i];
                break;
            }
            case BindingType::Texture: {
                auto* texture = static_cast<VulkanTexture*>(entry.texture.get());
                KUMO_ASSERT(texture != nullptr);
                imageInfos[i] = {VK_NULL_HANDLE, texture->view(),
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                write.pImageInfo = &imageInfos[i];
                break;
            }
            case BindingType::StorageTexture: {
                auto* texture = static_cast<VulkanTexture*>(entry.texture.get());
                KUMO_ASSERT(texture != nullptr);
                imageInfos[i] = {VK_NULL_HANDLE, texture->view(), VK_IMAGE_LAYOUT_GENERAL};
                write.pImageInfo = &imageInfos[i];
                storageImages.push_back(texture->image());
                break;
            }
            case BindingType::Sampler: {
                auto* sampler = static_cast<VulkanSampler*>(entry.sampler.get());
                KUMO_ASSERT(sampler != nullptr);
                imageInfos[i] = {sampler->handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                write.pImageInfo = &imageInfos[i];
                break;
            }
            }
            writes.push_back(write);
        }

        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        return std::make_shared<VulkanBindGroup>(set, std::move(storageImages));
    }

    Ptr<RenderPipeline> createRenderPipeline(const RenderPipelineDesc& desc) override {
        KUMO_ASSERT(desc.pushConstantSize <= 128);
        KUMO_ASSERT(desc.sampleCount == 1 || desc.sampleCount == 4);
        if (!desc.vertexShader || !desc.fragmentShader) {
            logError("createRenderPipeline requires vertex and fragment shaders");
            return nullptr;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        auto* vertex = static_cast<VulkanShaderModule*>(desc.vertexShader.get());
        auto* fragment = static_cast<VulkanShaderModule*>(desc.fragmentShader.get());
        VkPipelineShaderStageCreateInfo vertexStage{};
        vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertex->handle();
        vertexStage.pName = vertex->entryPoint().c_str();
        stages.push_back(vertexStage);
        VkPipelineShaderStageCreateInfo fragmentStage{};
        fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragment->handle();
        fragmentStage.pName = fragment->entryPoint().c_str();
        stages.push_back(fragmentStage);

        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        for (std::size_t slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
            const VertexBufferLayout& layout = desc.vertexBuffers[slot];
            VkVertexInputBindingDescription binding{};
            binding.binding = static_cast<std::uint32_t>(slot);
            binding.stride = layout.stride;
            binding.inputRate = layout.stepMode == VertexStepMode::Vertex
                                    ? VK_VERTEX_INPUT_RATE_VERTEX
                                    : VK_VERTEX_INPUT_RATE_INSTANCE;
            vertexBindings.push_back(binding);
            for (const VertexAttribute& attribute : layout.attributes) {
                VkVertexInputAttributeDescription attr{};
                attr.location = attribute.shaderLocation;
                attr.binding = static_cast<std::uint32_t>(slot);
                attr.format = toVk(attribute.format);
                attr.offset = attribute.offset;
                vertexAttributes.push_back(attr);
            }
        }
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount =
            static_cast<std::uint32_t>(vertexBindings.size());
        vertexInput.pVertexBindingDescriptions = vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(vertexAttributes.size());
        vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = toVk(desc.topology);

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = desc.cullMode == CullMode::None    ? VK_CULL_MODE_NONE
                                 : desc.cullMode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                                    : VK_CULL_MODE_BACK_BIT;
        // Negative-height viewport flips framebuffer winding; invert front face so
        // culling matches Metal.
        rasterization.frontFace = desc.frontFace == FrontFace::CCW
                                      ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = toVkSamples(desc.sampleCount);

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const bool hasDepth = desc.depthStencil.format != TextureFormat::Undefined;
        depthStencil.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = toVk(desc.depthStencil.depthCompare);

        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        for (std::size_t i = 0; i < desc.colorFormats.size(); ++i) {
            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable = VK_FALSE;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blendAttachments.push_back(blend);
        }
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
        colorBlend.pAttachments = blendAttachments.data();

        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        std::vector<VkDescriptorSetLayout> setLayouts;
        for (const Ptr<BindGroupLayout>& bindGroupLayout : desc.bindGroupLayouts) {
            setLayouts.push_back(
                bindGroupLayout
                    ? static_cast<VulkanBindGroupLayout*>(bindGroupLayout.get())->handle()
                    : emptySetLayout_);
        }
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = desc.pushConstantSize;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
        layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            logError("createRenderPipeline: pipeline layout failed");
            return nullptr;
        }

        std::vector<VkFormat> colorFormats;
        for (TextureFormat format : desc.colorFormats) {
            colorFormats.push_back(toVk(format));
        }
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size());
        renderingInfo.pColorAttachmentFormats = colorFormats.data();
        renderingInfo.depthAttachmentFormat =
            hasDepth ? toVk(desc.depthStencil.format) : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                      &pipeline) != VK_SUCCESS) {
            logError("createRenderPipeline failed");
            vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
            return nullptr;
        }
        return std::make_shared<VulkanRenderPipeline>(device_, pipeline, pipelineLayout,
                                                      desc.sampleCount);
    }

    Ptr<ComputePipeline> createComputePipeline(const ComputePipelineDesc& desc) override {
        KUMO_ASSERT(desc.pushConstantSize <= 128);
        // Zero threadgroup dimensions hang the GPU on backends that dispatch with them.
        KUMO_ASSERT(desc.workgroupSizeX > 0 && desc.workgroupSizeY > 0 && desc.workgroupSizeZ > 0);
        if (!desc.shader) {
            logError("createComputePipeline requires a compute shader");
            return nullptr;
        }
        auto* shader = static_cast<VulkanShaderModule*>(desc.shader.get());

        std::vector<VkDescriptorSetLayout> setLayouts;
        for (const Ptr<BindGroupLayout>& bindGroupLayout : desc.bindGroupLayouts) {
            setLayouts.push_back(
                bindGroupLayout
                    ? static_cast<VulkanBindGroupLayout*>(bindGroupLayout.get())->handle()
                    : emptySetLayout_);
        }
        // Compute pipeline layouts need the COMPUTE stage flag on their push range.
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = desc.pushConstantSize;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
        layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            logError("createComputePipeline: pipeline layout failed");
            return nullptr;
        }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader->handle();
        stage.pName = shader->entryPoint().c_str();

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                     &pipeline) != VK_SUCCESS) {
            logError("createComputePipeline failed");
            vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
            return nullptr;
        }
        return std::make_shared<VulkanComputePipeline>(device_, pipeline, pipelineLayout);
    }

    Ptr<Surface> createSurface(const SurfaceDesc& desc) override {
        KUMO_ASSERT(desc.nativeSurface != nullptr);
        return std::make_shared<VulkanSurface>(instance_, physical_, device_, graphicsFamily_,
                                               reinterpret_cast<VkSurfaceKHR>(desc.nativeSurface),
                                               &queue_, &tracker_);
    }

    Queue& queue() override { return queue_; }

    NativeHandles nativeHandles() override {
        return {.device = nullptr,
                .vkInstance = instance_,
                .vkPhysicalDevice = physical_,
                .vkDevice = device_,
                .vkQueue = graphicsQueue_,
                .vkQueueFamily = graphicsFamily_};
    }

private:
    VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout) {
        if (descriptorPools_.empty()) {
            createDescriptorPool();
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descriptorPools_.back();
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &set);
            if (result == VK_SUCCESS) {
                return set;
            }
            if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
                createDescriptorPool();
                continue;
            }
            return VK_NULL_HANDLE;
        }
        return VK_NULL_HANDLE;
    }

    void createDescriptorPool() {
        constexpr std::uint32_t kSetsPerPool = 64;
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = kSetsPerPool;
        poolInfo.poolSizeCount = 5;
        poolInfo.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool) == VK_SUCCESS) {
            descriptorPools_.push_back(pool);
        }
    }

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

} // namespace

Ptr<Device> createDevice(const DeviceDesc& desc) {
    vkb::InstanceBuilder builder;
    builder.set_app_name("kumo").set_engine_name("kumo").require_api_version(1, 3, 0);
    if (desc.enableValidation) {
        builder.request_validation_layers(true)
            .set_debug_callback(
                [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
                    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                        logError("[vulkan] {}", data->pMessage);
                    } else {
                        logWarn("[vulkan] {}", data->pMessage);
                    }
                    return VK_FALSE;
                })
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
    }
    auto instanceResult = builder.build();
    if (!instanceResult) {
        logError("vulkan instance creation failed: {}", instanceResult.error().message());
        return nullptr;
    }
    vkb::Instance vkbInstance = instanceResult.value();

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures features10{};
    features10.samplerAnisotropy = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{vkbInstance};
    auto physicalResult = selector.set_minimum_version(1, 3)
                              .set_required_features(features10)
                              .set_required_features_13(features13)
                              .defer_surface_initialization()
                              .select();
    if (!physicalResult) {
        logError("vulkan physical device selection failed: {}", physicalResult.error().message());
        vkb::destroy_instance(vkbInstance);
        return nullptr;
    }
    vkb::PhysicalDevice vkbPhysical = physicalResult.value();
    vkbPhysical.enable_extension_if_present("VK_KHR_portability_subset");

    vkb::DeviceBuilder deviceBuilder{vkbPhysical};
    auto deviceResult = deviceBuilder.build();
    if (!deviceResult) {
        logError("vulkan device creation failed: {}", deviceResult.error().message());
        vkb::destroy_instance(vkbInstance);
        return nullptr;
    }
    vkb::Device vkbDevice = deviceResult.value();

    auto queueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    auto familyResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!queueResult || !familyResult) {
        logError("vulkan graphics queue unavailable");
        vkb::destroy_device(vkbDevice);
        vkb::destroy_instance(vkbInstance);
        return nullptr;
    }

    logInfo("Vulkan device: {}", vkbPhysical.name);

    auto device = std::make_shared<VulkanDevice>(vkbInstance, vkbPhysical, vkbDevice,
                                                 queueResult.value(), familyResult.value());
    if (!device->ok()) {
        logError("vulkan device initialization failed");
        return nullptr;
    }
    return device;
}

} // namespace kumo::rhi::vulkan
