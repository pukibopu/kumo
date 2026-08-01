#pragma once

#include <cstdint>

namespace kumo::shaderabi::metal {

// Metal argument table layout shared by shader translation and GPU encoding:
// flat resource indices 0..23 hold sets 0-2 (8 bindings each), push constants
// sit at buffer index 24, and vertex streams occupy indices 30 downwards.
inline constexpr std::uint32_t kMaxBindingsPerSet = 8;
inline constexpr std::uint32_t kMaxBindGroups = 3;
inline constexpr std::uint32_t kPushConstantBufferIndex = 24;
inline constexpr std::uint32_t kVertexBufferBaseIndex = 30;
inline constexpr std::uint32_t kMaxVertexBufferSlots = 5;

// Metal's sampler argument table has only 16 entries, so samplers use a denser
// stride-6 mapping; the shader compiler rejects sampler indices above 15.
inline constexpr std::uint32_t kSamplerTableStride = 6;
inline constexpr std::uint32_t kMaxSamplerIndex = 15;

constexpr std::uint32_t resourceIndex(std::uint32_t set, std::uint32_t binding) {
    return set * kMaxBindingsPerSet + binding;
}

constexpr std::uint32_t samplerIndex(std::uint32_t set, std::uint32_t binding) {
    return set * kSamplerTableStride + binding;
}

constexpr std::uint32_t vertexBufferIndex(std::uint32_t slot) {
    return kVertexBufferBaseIndex - slot;
}

} // namespace kumo::shaderabi::metal
