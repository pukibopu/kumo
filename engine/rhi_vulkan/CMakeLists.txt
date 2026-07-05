add_library(kumo_rhi_vulkan STATIC src/rhi_vulkan.cpp)
target_include_directories(kumo_rhi_vulkan PUBLIC include)
target_link_libraries(kumo_rhi_vulkan
    PUBLIC kumo_rhi
    PRIVATE kumo_core Vulkan::Vulkan vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
)
kumo_target_warnings(kumo_rhi_vulkan)

if(APPLE)
    # libvulkan (MoltenVK loader) ships as @rpath/libvulkan.1.dylib; point the
    # rpath at the SDK lib dir so linked binaries resolve it at runtime.
    get_filename_component(_kumo_vulkan_libdir "${Vulkan_LIBRARY}" DIRECTORY)
    set_target_properties(kumo_rhi_vulkan PROPERTIES BUILD_RPATH "${_kumo_vulkan_libdir}")
endif()
