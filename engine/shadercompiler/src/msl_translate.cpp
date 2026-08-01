#include "internal.h"

#include <kumo/shaderabi/metal_binding.h>

#include <spirv_msl.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <exception>
#include <format>
#include <string_view>
#include <tuple>

namespace kumo::shaderc::detail {
namespace {

namespace metalBinding = shaderabi::metal;

spirv_cross::CompilerMSL::Options::Platform toMslPlatform(MslPlatform platform) {
    using Platform = spirv_cross::CompilerMSL::Options::Platform;

    switch (platform) {
    case MslPlatform::MacOS:
        return Platform::macOS;
    case MslPlatform::IOS:
        return Platform::iOS;
    case MslPlatform::Native:
#if defined(__APPLE__) && TARGET_OS_IPHONE
        return Platform::iOS;
#else
        return Platform::macOS;
#endif
    }
    return Platform::macOS;
}

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
             spv::ExecutionModel model, Reflection& reflection, bool isSampler = false) {
    const bool isBuffer =
        std::string_view(type) == "uniform_buffer" || std::string_view(type) == "storage_buffer";
    for (const spirv_cross::Resource& res : list) {
        const std::uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        const std::uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);
        const std::uint32_t bufferSize =
            isBuffer ? static_cast<std::uint32_t>(
                           compiler.get_declared_struct_size(compiler.get_type(res.base_type_id)))
                     : 0;
        reflection.bindings.push_back({.set = set,
                                       .binding = binding,
                                       .type = type,
                                       .name = res.name,
                                       .bufferSize = bufferSize});

        const std::uint32_t flat = metalBinding::resourceIndex(set, binding);
        spirv_cross::MSLResourceBinding remap{};
        remap.stage = model;
        remap.desc_set = set;
        remap.binding = binding;
        remap.msl_buffer = flat;
        remap.msl_texture = flat;
        remap.msl_sampler = isSampler ? metalBinding::samplerIndex(set, binding) : flat;
        compiler.add_msl_resource_binding(remap);
    }
}

std::vector<CompileError> validateBindingRanges(const Reflection& reflection) {
    std::vector<CompileError> errors;
    for (const ReflectionBinding& b : reflection.bindings) {
        if (b.set >= metalBinding::kMaxBindGroups ||
            b.binding >= metalBinding::kMaxBindingsPerSet) {
            errors.push_back(CompileError{
                .file = {},
                .line = 0,
                .message = std::format("binding out of range: set={} binding={} (sets 0-{}, "
                                       "bindings 0-{})",
                                       b.set, b.binding, metalBinding::kMaxBindGroups - 1,
                                       metalBinding::kMaxBindingsPerSet - 1),
                .secondStage = true,
            });
        } else if (b.type == "sampler" &&
                   metalBinding::samplerIndex(b.set, b.binding) > metalBinding::kMaxSamplerIndex) {
            errors.push_back(CompileError{
                .file = {},
                .line = 0,
                .message =
                    std::format("sampler binding out of Metal's 16-slot table: set={} binding={}",
                                b.set, b.binding),
                .secondStage = true,
            });
        }
    }
    return errors;
}

} // namespace

std::vector<CompileError> translateToMsl(CompiledShader& shader, Stage stage,
                                         MslPlatform platform) {
    try {
        spirv_cross::CompilerMSL compiler(shader.spirv);

        spirv_cross::CompilerMSL::Options options;
        options.platform = toMslPlatform(platform);
        options.set_msl_version(2, 4);
        compiler.set_msl_options(options);

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        const spv::ExecutionModel model = toExecutionModel(stage);

        collect(compiler, resources.uniform_buffers, "uniform_buffer", model, shader.reflection);
        collect(compiler, resources.storage_buffers, "storage_buffer", model, shader.reflection);
        collect(compiler, resources.separate_images, "sampled_texture", model, shader.reflection);
        collect(compiler, resources.separate_samplers, "sampler", model, shader.reflection, true);
        collect(compiler, resources.storage_images, "storage_texture", model, shader.reflection);

        if (std::vector<CompileError> violations = validateBindingRanges(shader.reflection);
            !violations.empty()) {
            return violations;
        }

        if (!resources.push_constant_buffers.empty()) {
            const spirv_cross::Resource& pc = resources.push_constant_buffers.front();
            shader.reflection.pushConstantSize = static_cast<std::uint32_t>(
                compiler.get_declared_struct_size(compiler.get_type(pc.base_type_id)));

            spirv_cross::MSLResourceBinding remap{};
            remap.stage = model;
            remap.desc_set = spirv_cross::kPushConstDescSet;
            remap.binding = spirv_cross::kPushConstBinding;
            remap.msl_buffer = metalBinding::kPushConstantBufferIndex;
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

        if (stage == Stage::Compute && !entryPoints.empty()) {
            const auto& wg =
                compiler
                    .get_entry_point(entryPoints.front().name, entryPoints.front().execution_model)
                    .workgroup_size;
            shader.reflection.workgroupSize[0] = wg.x;
            shader.reflection.workgroupSize[1] = wg.y;
            shader.reflection.workgroupSize[2] = wg.z;
        }
        return {};
    } catch (const std::exception& e) {
        return {CompileError{.file = {}, .line = 0, .message = e.what(), .secondStage = true}};
    } catch (...) {
        return {CompileError{
            .file = {}, .line = 0, .message = "unknown SPIRV-Cross error", .secondStage = true}};
    }
}

} // namespace kumo::shaderc::detail
