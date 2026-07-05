#pragma once

#include <kumo/shaderc/compiler.h>

#include <optional>

// glslang and SPIRV-Cross both define namespace spv with incompatible contents,
// so their uses live in separate translation units bridged by this interface.
namespace kumo::shaderc::detail {

std::optional<CompileError> translateToMsl(CompiledShader& shader, Stage stage);

} // namespace kumo::shaderc::detail
