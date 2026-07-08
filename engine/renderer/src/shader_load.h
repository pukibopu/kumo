#pragma once

#include <kumo/rhi/rhi.h>
#include <kumo/shaderc/compiler.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kumo::renderer::detail {

struct CompiledStage {
    rhi::Ptr<rhi::ShaderModule> module;
    shaderc::Reflection reflection;
};

// Compiles shaders/<file> from KUMO_SHADER_DIR (with shaders/include on the
// include path) and creates the module for the device's backend. Errors are
// logged with file/line; nullopt on failure.
std::optional<CompiledStage> loadStage(rhi::Device& device, const char* file,
                                       shaderc::Stage stage);

struct StageReflection {
    const shaderc::Reflection* reflection = nullptr;
    rhi::ShaderStage stage = rhi::ShaderStage::None;
};

// Bind group layouts derived from reflection, merged across stages (ADR 0040);
// indexed by set number with nullptr for empty sets.
std::vector<rhi::Ptr<rhi::BindGroupLayout>>
layoutsFromReflection(rhi::Device& device, std::span<const StageReflection> stages);

// Stable text form of the merged binding tables; a hot reload that changes it
// would orphan existing bind groups, so callers compare before swapping.
std::string layoutSignature(std::span<const StageReflection> stages);

} // namespace kumo::renderer::detail
