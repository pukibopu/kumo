#pragma once

#include "kumo/rhi/rhi.h"

namespace kumo::rhi::vulkan {

// The VkInstance is exposed via Device::nativeHandles() after creation; the app
// builds a VkSurfaceKHR from it (glfwCreateWindowSurface) and passes it back
// through SurfaceDesc.nativeSurface.
Ptr<Device> createDevice(const DeviceDesc& desc);

} // namespace kumo::rhi::vulkan
