#pragma once

#include <kumo/gpu/gpu.h>
#include <kumo/shaderc/compiler.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::renderer::detail {

struct CompiledStage {
    gpu::Ptr<gpu::ShaderModule> module;
    shaderc::Reflection reflection;
};

// Compiles shaders/<file> from KUMO_SHADER_DIR (with shaders/include on the
// include path) and creates the module for the device's backend. Errors are
// logged with file/line; nullopt on failure.
std::optional<CompiledStage> loadStage(gpu::Device& device, const char* file, shaderc::Stage stage);

struct StageReflection {
    const shaderc::Reflection* reflection = nullptr;
    gpu::ShaderStage stage = gpu::ShaderStage::None;
};

// Bind group layouts derived from reflection, merged across stages (ADR 0040);
// indexed by set number with nullptr for empty sets.
std::vector<gpu::Ptr<gpu::BindGroupLayout>>
layoutsFromReflection(gpu::Device& device, std::span<const StageReflection> stages);

// Stable text form of the merged binding tables ("set:binding:type:visibility:size;"
// per binding); a hot reload that changes it would orphan existing bind groups, so
// callers compare before swapping. Buffer size is part of the signature (ADR 0043)
// so a shader edit that resizes a uniform/storage block without touching its
// set/binding/type still invalidates cached layouts instead of silently mismatching
// the CPU-side buffer.
std::string layoutSignature(std::span<const StageReflection> stages);

// Checks the material-agent binding compatibility contract for a single set
// (ADR 0011/0029): a material's custom fragment shader may only differ from
// the shared pbr fragment shader in set 1 (its own factors block), so sets 0
// and 2 must reflect identically between `custom` and `shared` (same bindings,
// types and declared buffer sizes). Returns a human-readable description of
// the first mismatch found (missing/extra binding, type, or buffer size),
// nullopt when the set is compatible.
std::optional<std::string> setMismatch(const shaderc::Reflection& custom,
                                       const shaderc::Reflection& shared, std::uint32_t set);

// Decides how many bytes of MaterialFactorsData's engine-written prefix
// (baseColor, metallicRoughness, emissive, uvTiling — 64B total) a material's
// set-1 factors block accepts. Declared byte size alone cannot tell an
// appended custom member from uvTiling: std140 rounds a block up in 16B
// steps, so an old shader with its own vec4 member after emissive reflects
// the same 64B bufferSize as the current uvTiling layout. This checks the
// block's 4th member name instead; only "uvTiling" there unlocks the full
// 64B write, otherwise (including blocks with fewer than 4 members) only the
// 48B baseColor/metallicRoughness/emissive prefix is written. Either way the
// result is clamped to bufferSize, so it never writes past what the shader
// itself declared.
std::uint32_t factorPrefixSize(const shaderc::ReflectionBinding& factors);

// True when `source` textually references frame.timeParams (a substring match,
// including inside comments — a false positive there only costs a spurious
// redraw). Drives ForwardRenderer::hasAnimatedMaterials so the app keeps
// redrawing an on-demand viewport while an animated material is installed.
bool sourceReferencesTime(std::string_view source);

} // namespace kumo::renderer::detail
