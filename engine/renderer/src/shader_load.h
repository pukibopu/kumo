#pragma once

#include <kumo/gpu/gpu.h>
#include <kumo/shaderc/compiler.h>

#include <optional>
#include <span>
#include <string>
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

} // namespace kumo::renderer::detail
