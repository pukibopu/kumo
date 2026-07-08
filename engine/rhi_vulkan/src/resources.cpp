#include "internal.h"

namespace kumo::rhi::vulkan {

Ptr<Buffer> VulkanDevice::createBuffer(const BufferDesc& desc) {
    KUMO_ASSERT(desc.size > 0);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = toVkBufferUsage(desc.usage);

    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
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

Ptr<Texture> VulkanDevice::createTexture(const TextureDesc& desc) {
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
    viewInfo.viewType =
        desc.dimension == TextureDimension::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = toVk(desc.format);
    viewInfo.subresourceRange = {static_cast<VkImageAspectFlags>(isDepthFormat(desc.format)
                                                                     ? VK_IMAGE_ASPECT_DEPTH_BIT
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

Ptr<Texture> VulkanDevice::createTextureView(const Ptr<Texture>& texture,
                                             const TextureViewDesc& desc) {
    KUMO_ASSERT(texture != nullptr);
    auto* parent = static_cast<VulkanTexture*>(texture.get());
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = parent->image();
    viewInfo.viewType =
        desc.dimension == TextureDimension::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
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

Ptr<Sampler> VulkanDevice::createSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = toVkFilter(desc.magFilter);
    samplerInfo.minFilter = toVkFilter(desc.minFilter);
    samplerInfo.mipmapMode = desc.mipFilter == FilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
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

Ptr<ShaderModule> VulkanDevice::createShaderModule(const ShaderModuleDesc& desc) {
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
    return std::make_shared<VulkanShaderModule>(device_, module,
                                                desc.entryPoint.empty() ? "main" : desc.entryPoint);
}

Ptr<BindGroupLayout> VulkanDevice::createBindGroupLayout(const BindGroupLayoutDesc& desc) {
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

Ptr<BindGroup> VulkanDevice::createBindGroup(const BindGroupDesc& desc) {
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

VkDescriptorSet VulkanDevice::allocateDescriptorSet(VkDescriptorSetLayout layout) {
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

void VulkanDevice::createDescriptorPool() {
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

} // namespace kumo::rhi::vulkan
