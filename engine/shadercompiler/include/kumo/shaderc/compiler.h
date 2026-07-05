#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::shaderc {

enum class Stage { Vertex, Fragment, Compute };

struct CompileError {
    std::string file; // "" for the main source, else include name
    int line = 0;     // 0 when unknown
    std::string message;
    bool secondStage = false; // SPIRV-Cross / MSL-translation error
};

struct ReflectionBinding {
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    // "uniform_buffer" | "storage_buffer" | "sampled_texture" | "sampler"
    std::string type;
    std::string name;
};

struct Reflection {
    std::vector<ReflectionBinding> bindings; // sorted by (set, binding)
    std::uint32_t pushConstantSize = 0;
    std::vector<std::uint32_t> vertexInputLocations; // sorted, vertex stage only
};

struct CompiledShader {
    std::vector<std::uint32_t> spirv;
    std::string msl;
    std::string mslEntryPoint; // SPIRV-Cross renames main -> main0
    Reflection reflection;
};

struct CompileOptions {
    std::string sourceName = "shader";    // used in error messages
    std::vector<std::string> includeDirs; // for GL_GOOGLE_include_directive
};

using CompileResult = std::expected<CompiledShader, std::vector<CompileError>>;

CompileResult compileGlsl(std::string_view source, Stage stage, const CompileOptions& options = {});

} // namespace kumo::shaderc
