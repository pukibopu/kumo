#pragma once

#include <kumo/shaderc/compiler.h>

#include <vector>

// glslang and SPIRV-Cross both define namespace spv with incompatible contents,
// so their uses live in separate translation units bridged by this interface.
namespace kumo::shaderc::detail {

// Empty on success; otherwise the translation/validation errors.
std::vector<CompileError> translateToMsl(CompiledShader& shader, Stage stage, MslPlatform platform);

} // namespace kumo::shaderc::detail
