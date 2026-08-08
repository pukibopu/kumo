#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::agent::surface {

// Deterministic, CPU-side splicing of a surface function and its parameters
// into shaders/pbr_surface_template.frag. Offsets are computed here (std140,
// engine-authoritative); the shader block is generated to match them.

inline constexpr std::size_t kMaxParams = 16;
inline constexpr std::uint32_t kPrefixSize = 64; // four fixed vec4 members
inline constexpr std::uint32_t kMaxBlockSize = 192;

struct ParamDecl {
    std::string name;
    bool isVec4 = false;
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // default; floats use [0]
};

struct ParamSlot {
    std::string name;
    bool isVec4 = false;
    std::uint32_t offset = 0; // bytes from the block start
};

struct SplicedSurface {
    std::string source;
    std::vector<ParamSlot> layout;
    std::uint32_t dataSize = 0; // param bytes after the prefix
    int functionFirstLine = 0;  // 1-based, in `source`
    int functionLineCount = 0;
};

// The construct that bans this function from the surface path, or nullopt.
std::optional<std::string> findForbiddenConstruct(std::string_view functionSource);

// Validates names and computes std140 offsets; shared by spliceSurface and
// the scene-load path (offsets are never persisted, only order+type).
std::expected<std::vector<ParamSlot>, std::string> computeLayout(std::span<const ParamDecl> params);

// Validates params and the function signature, computes the std140 layout and
// splices both template markers. The template text must contain
// //KUMO_SURFACE_PARAMS and //KUMO_SURFACE_FUNCTION.
std::expected<SplicedSurface, std::string> spliceSurface(std::string_view templateText,
                                                         std::string_view functionSource,
                                                         std::span<const ParamDecl> params);

// Default-value bytes matching `layout` (same order as the decls it came from).
std::vector<std::byte> packDefaults(std::span<const ParamDecl> params,
                                    std::span<const ParamSlot> layout);

// Function-relative line for a compile error in the spliced source; nullopt
// when the error lies in the template or parameter zone.
std::optional<int> functionLine(const SplicedSurface& spliced, int compiledLine);

} // namespace kumo::agent::surface
