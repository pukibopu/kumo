#include "internal.h"

#include <spirv_msl.hpp>

#include <algorithm>
#include <exception>
#include <tuple>

namespace kumo::shaderc::detail {
namespace {

// Mirror of kumo::rhi::metal in engine/rhi_metal/include/kumo/rhi_metal/binding_map.h.
// Duplicated locally so shaderc does not depend on the Metal backend module.
constexpr std::uint32_t kMaxBindingsPerSet = 8;
constexpr std::uint32_t kPushConstantBufferIndex = 24;

spv::ExecutionModel toExecutionModel(Stage stage) {
    switch (stage) {
    case Stage::Vertex:
        return spv::ExecutionModelVertex;
    case Stage::Fragment:
        return spv::ExecutionModelFragment;
    case Stage::Compute:
        return spv::ExecutionModelGLCompute;
    }
    return spv::ExecutionModelVertex;
}

void collect(spirv_cross::CompilerMSL& compiler,
             const spirv_cross::SmallVector<spirv_cross::Resource>& list, const char* type,
             spv::ExecutionModel model, Reflection& reflection) {
    for (const spirv_cross::Resource& res : list) {
        const std::uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        const std::uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
        reflection.bindings.push_back(
            {.set = set, .binding = binding, .type = type, .name = res.name});

        const std::uint32_t flat = set * kMaxBindingsPerSet + binding;
        spirv_cross::MSLResourceBinding remap{};
        remap.stage = model;
        remap.desc_set = set;
        remap.binding = binding;
        remap.msl_buffer = flat;
        remap.msl_texture = flat;
        remap.msl_sampler = flat;
        compiler.add_msl_resource_binding(remap);
    }
}

} // namespace

std::optional<CompileError> translateToMsl(CompiledShader& shader, Stage stage) {
    try {
        spirv_cross::CompilerMSL compiler(shader.spirv);

        spirv_cross::CompilerMSL::Options options;
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        options.set_msl_version(2, 4);
        compiler.set_msl_options(options);

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        const spv::ExecutionModel model = toExecutionModel(stage);

        collect(compiler, resources.uniform_buffers, "uniform_buffer", model, shader.reflection);
        collect(compiler, resources.storage_buffers, "storage_buffer", model, shader.reflection);
        collect(compiler, resources.separate_images, "sampled_texture", model, shader.reflection);
        collect(compiler, resources.separate_samplers, "sampler", model, shader.reflection);

        if (!resources.push_constant_buffers.empty()) {
            const spirv_cross::Resource& pc = resources.push_constant_buffers.front();
            shader.reflection.pushConstantSize = static_cast<std::uint32_t>(
                compiler.get_declared_struct_size(compiler.get_type(pc.base_type_id)));

            spirv_cross::MSLResourceBinding remap{};
            remap.stage = model;
            remap.desc_set = spirv_cross::kPushConstDescSet;
            remap.binding = spirv_cross::kPushConstBinding;
            remap.msl_buffer = kPushConstantBufferIndex;
            compiler.add_msl_resource_binding(remap);
        }

        if (stage == Stage::Vertex) {
            for (const spirv_cross::Resource& in : resources.stage_inputs) {
                shader.reflection.vertexInputLocations.push_back(
                    compiler.get_decoration(in.id, spv::DecorationLocation));
            }
        }

        std::sort(shader.reflection.bindings.begin(), shader.reflection.bindings.end(),
                  [](const ReflectionBinding& a, const ReflectionBinding& b) {
                      return std::tie(a.set, a.binding) < std::tie(b.set, b.binding);
                  });
        std::sort(shader.reflection.vertexInputLocations.begin(),
                  shader.reflection.vertexInputLocations.end());

        shader.msl = compiler.compile();

        const auto entryPoints = compiler.get_entry_points_and_stages();
        shader.mslEntryPoint =
            entryPoints.empty()
                ? "main0"
                : compiler.get_cleansed_entry_point_name(entryPoints.front().name,
                                                         entryPoints.front().execution_model);
        return std::nullopt;
    } catch (const std::exception& e) {
        return CompileError{.file = {}, .line = 0, .message = e.what(), .secondStage = true};
    } catch (...) {
        return CompileError{
            .file = {}, .line = 0, .message = "unknown SPIRV-Cross error", .secondStage = true};
    }
}

} // namespace kumo::shaderc::detail
