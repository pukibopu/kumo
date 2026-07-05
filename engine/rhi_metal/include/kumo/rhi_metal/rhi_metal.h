#pragma once

#include "kumo/rhi/rhi.h"

namespace kumo::rhi::metal {

Ptr<Device> createDevice(const DeviceDesc& desc);

} // namespace kumo::rhi::metal
