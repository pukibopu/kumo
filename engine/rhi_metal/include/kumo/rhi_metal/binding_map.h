#pragma once

#include <cstdint>

namespace kumo::rhi::metal {

// Metal argument table layout shared with the SPIRV-Cross remap in M3:
// flat resource indices 0..23 hold sets 0-2 (8 bindings each), push constants
// sit at buffer index 24, vertex streams occupy indices 30 downwards.
inline constexpr std::uint32_t kMaxBindingsPerSet = 8;
inline constexpr std::uint32_t kMaxBindGroups = 3;
inline constexpr std::uint32_t kPushConstantBufferIndex = 24;
inline constexpr std::uint32_t kVertexBufferBaseIndex = 30;
inline constexpr std::uint32_t kMaxVertexBufferSlots = 5;

constexpr std::uint32_t resourceIndex(std::uint32_t set, std::uint32_t binding) {
    return set * kMaxBindingsPerSet + binding;
}

constexpr std::uint32_t vertexBufferIndex(std::uint32_t slot) {
    return kVertexBufferBaseIndex - slot;
}

} // namespace kumo::rhi::metal
