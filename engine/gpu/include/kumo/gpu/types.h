#pragma once

#include <cstdint>

namespace kumo::gpu {

enum class TextureFormat {
    Undefined,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    BGRA8Unorm,
    BGRA8UnormSrgb,
    RGBA16Float,
    RG16Float,
    R32Float,
    RGBA32Float,
    Depth32Float,
};

enum class TextureDimension { Tex2D, Cube };

// Automatic preserves the backend's normal shared/private placement policy.
// TransientAttachment permits memoryless placement when the descriptor and GPU
// support it, and otherwise falls back to private storage.
enum class StorageMode { Automatic, TransientAttachment };

enum class BufferUsage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
};

enum class TextureUsage : std::uint32_t {
    None = 0,
    Sampled = 1u << 0,
    RenderTarget = 1u << 1,
    Storage = 1u << 2,
    CopySrc = 1u << 3,
    CopyDst = 1u << 4,
};

enum class ShaderStage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Fragment = 1u << 1,
    Compute = 1u << 2,
};

#define KUMO_GPU_ENUM_FLAGS(E)                                                                     \
    constexpr E operator|(E a, E b) {                                                              \
        return static_cast<E>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));      \
    }                                                                                              \
    constexpr E operator&(E a, E b) {                                                              \
        return static_cast<E>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));      \
    }                                                                                              \
    constexpr bool hasFlag(E value, E flag) {                                                      \
        return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;        \
    }

KUMO_GPU_ENUM_FLAGS(BufferUsage)
KUMO_GPU_ENUM_FLAGS(TextureUsage)
KUMO_GPU_ENUM_FLAGS(ShaderStage)

#undef KUMO_GPU_ENUM_FLAGS

enum class LoadOp { Load, Clear, DontCare };
enum class StoreOp { Store, DontCare };

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class FilterMode { Nearest, Linear };
enum class AddressMode { Repeat, MirrorRepeat, ClampToEdge };

enum class PrimitiveTopology { TriangleList, TriangleStrip, LineList };
enum class CullMode { None, Front, Back };
enum class FrontFace { CCW, CW };
enum class IndexFormat { Uint16, Uint32 };

enum class VertexFormat { Float32, Float32x2, Float32x3, Float32x4, Uint32, Unorm8x4 };
enum class VertexStepMode { Vertex, Instance };

enum class BindingType { UniformBuffer, StorageBuffer, Texture, StorageTexture, Sampler };

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

} // namespace kumo::gpu
