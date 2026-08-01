#pragma once

#include "kumo/gpu/gpu.h"

namespace CA {
class MetalLayer;
}

namespace MTL {
class CommandBuffer;
class Device;
class RenderCommandEncoder;
class RenderPassDescriptor;
} // namespace MTL

namespace kumo::gpu::metal {

// The layer is retained by the Surface. Drawable readback disables Metal's
// framebuffer-only optimization and should only be enabled by tools that need it.
[[nodiscard]] Ptr<Surface> createSurface(Device& device, CA::MetalLayer* layer,
                                         bool allowDrawableReadback = false);

[[nodiscard]] MTL::Device* nativeDevice(Device& device) noexcept;
[[nodiscard]] MTL::CommandBuffer* nativeCommandBuffer(CommandEncoder& encoder) noexcept;
[[nodiscard]] MTL::RenderCommandEncoder*
nativeRenderCommandEncoder(RenderPassEncoder& encoder) noexcept;
[[nodiscard]] MTL::RenderPassDescriptor*
nativeRenderPassDescriptor(RenderPassEncoder& encoder) noexcept;

} // namespace kumo::gpu::metal
