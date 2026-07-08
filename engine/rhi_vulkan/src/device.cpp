#define VMA_IMPLEMENTATION
#include "internal.h"

namespace kumo::rhi::vulkan {

VulkanDevice::VulkanDevice(vkb::Instance instance, vkb::PhysicalDevice physical, vkb::Device device,
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
    if (vkCreateDescriptorSetLayout(device_, &emptyInfo, nullptr, &emptySetLayout_) != VK_SUCCESS) {
        logError("vkCreateDescriptorSetLayout (empty) failed");
        return;
    }

    ok_ = queue_.init(physical_, device_, graphicsQueue_, graphicsFamily_, allocator_, uploadPool_,
                      &tracker_);
}

VulkanDevice::~VulkanDevice() {
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

Ptr<Surface> VulkanDevice::createSurface(const SurfaceDesc& desc) {
    KUMO_ASSERT(desc.nativeSurface != nullptr);
    return std::make_shared<VulkanSurface>(instance_, physical_, device_, graphicsFamily_,
                                           reinterpret_cast<VkSurfaceKHR>(desc.nativeSurface),
                                           &queue_, &tracker_);
}

NativeHandles VulkanDevice::nativeHandles() {
    return {.device = nullptr,
            .vkInstance = instance_,
            .vkPhysicalDevice = physical_,
            .vkDevice = device_,
            .vkQueue = graphicsQueue_,
            .vkQueueFamily = graphicsFamily_};
}

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
