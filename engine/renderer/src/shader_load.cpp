#include "shader_load.h"

#include <kumo/core/file.h>
#include <kumo/core/log.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <utility>

namespace kumo::renderer::detail {

namespace {

rhi::ShaderStage toRhiStage(shaderc::Stage stage) {
    switch (stage) {
    case shaderc::Stage::Vertex:
        return rhi::ShaderStage::Vertex;
    case shaderc::Stage::Fragment:
        return rhi::ShaderStage::Fragment;
    case shaderc::Stage::Compute:
        return rhi::ShaderStage::Compute;
    }
    return rhi::ShaderStage::None;
}

std::optional<rhi::BindingType> toBindingType(const std::string& type) {
    if (type == "uniform_buffer") {
        return rhi::BindingType::UniformBuffer;
    }
    if (type == "storage_buffer") {
        return rhi::BindingType::StorageBuffer;
    }
    if (type == "sampled_texture") {
        return rhi::BindingType::Texture;
    }
    if (type == "storage_texture") {
        return rhi::BindingType::StorageTexture;
    }
    if (type == "sampler") {
        return rhi::BindingType::Sampler;
    }
    return std::nullopt;
}

struct MergedBinding {
    rhi::BindingType type = rhi::BindingType::UniformBuffer;
    rhi::ShaderStage visibility = rhi::ShaderStage::None;
    std::uint32_t bufferSize = 0;
};

std::map<std::pair<std::uint32_t, std::uint32_t>, MergedBinding>
mergeBindings(std::span<const StageReflection> stages) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, MergedBinding> merged;
    for (const StageReflection& stage : stages) {
        for (const shaderc::ReflectionBinding& binding : stage.reflection->bindings) {
            const auto type = toBindingType(binding.type);
            if (!type) {
                logError("unknown reflection binding type '{}' (set={} binding={})", binding.type,
                         binding.set, binding.binding);
                continue;
            }
            MergedBinding& entry = merged[{binding.set, binding.binding}];
            entry.type = *type;
            entry.visibility = entry.visibility | stage.stage;
            entry.bufferSize = binding.bufferSize;
        }
    }
    return merged;
}

} // namespace

std::optional<CompiledStage> loadStage(rhi::Device& device, const char* file,
                                       shaderc::Stage stage) {
    const std::filesystem::path path = std::filesystem::path(KUMO_SHADER_DIR) / file;
    auto source = readTextFile(path);
    if (!source) {
        logError("{}: {}", path.string(), source.error());
        return std::nullopt;
    }
    auto compiled = shaderc::compileGlsl(
        *source, stage,
        {.sourceName = file,
         .includeDirs = {(std::filesystem::path(KUMO_SHADER_DIR) / "include").string()}});
    if (!compiled) {
        for (const shaderc::CompileError& error : compiled.error()) {
            logError("{}", shaderc::formatError(error, file));
        }
        return std::nullopt;
    }

    rhi::Ptr<rhi::ShaderModule> module = device.createShaderModule({
        .stage = toRhiStage(stage),
        .language = rhi::ShaderSourceLanguage::MSL,
        .source = compiled->msl,
        .entryPoint = compiled->mslEntryPoint,
    });
    if (!module) {
        return std::nullopt;
    }
    return CompiledStage{std::move(module), std::move(compiled->reflection)};
}

std::vector<rhi::Ptr<rhi::BindGroupLayout>>
layoutsFromReflection(rhi::Device& device, std::span<const StageReflection> stages) {
    const auto merged = mergeBindings(stages);
    std::uint32_t setCount = 0;
    for (const auto& [key, binding] : merged) {
        setCount = std::max(setCount, key.first + 1);
    }

    std::vector<rhi::Ptr<rhi::BindGroupLayout>> layouts(setCount);
    for (std::uint32_t set = 0; set < setCount; ++set) {
        rhi::BindGroupLayoutDesc desc;
        for (const auto& [key, binding] : merged) {
            if (key.first == set) {
                desc.entries.push_back({.binding = key.second,
                                        .visibility = binding.visibility,
                                        .type = binding.type});
            }
        }
        if (!desc.entries.empty()) {
            layouts[set] = device.createBindGroupLayout(desc);
            if (!layouts[set]) {
                return {};
            }
        }
    }
    return layouts;
}

std::string layoutSignature(std::span<const StageReflection> stages) {
    std::string out;
    for (const auto& [key, binding] : mergeBindings(stages)) {
        out += std::format("{}:{}:{}:{}:{};", key.first, key.second,
                           static_cast<std::uint32_t>(binding.type),
                           static_cast<std::uint32_t>(binding.visibility), binding.bufferSize);
    }
    return out;
}

std::optional<std::string> setMismatch(const shaderc::Reflection& custom,
                                       const shaderc::Reflection& shared, std::uint32_t set) {
    std::map<std::uint32_t, const shaderc::ReflectionBinding*> customBindings;
    std::map<std::uint32_t, const shaderc::ReflectionBinding*> sharedBindings;
    for (const shaderc::ReflectionBinding& binding : custom.bindings) {
        if (binding.set == set) {
            customBindings[binding.binding] = &binding;
        }
    }
    for (const shaderc::ReflectionBinding& binding : shared.bindings) {
        if (binding.set == set) {
            sharedBindings[binding.binding] = &binding;
        }
    }

    for (const auto& [bindingIndex, sharedBinding] : sharedBindings) {
        const auto it = customBindings.find(bindingIndex);
        if (it == customBindings.end()) {
            return std::format("set {} binding {} ({}) is missing", set, bindingIndex,
                               sharedBinding->type);
        }
        const shaderc::ReflectionBinding* customBinding = it->second;
        if (customBinding->type != sharedBinding->type) {
            return std::format("set {} binding {} type changed from {} to {}", set, bindingIndex,
                               sharedBinding->type, customBinding->type);
        }
        if (sharedBinding->bufferSize != customBinding->bufferSize) {
            return std::format("set {} binding {} buffer size changed from {} to {}", set,
                               bindingIndex, sharedBinding->bufferSize, customBinding->bufferSize);
        }
    }
    for (const auto& [bindingIndex, customBinding] : customBindings) {
        if (!sharedBindings.contains(bindingIndex)) {
            return std::format("set {} binding {} ({}) is not part of the shared pipeline", set,
                               bindingIndex, customBinding->type);
        }
    }
    return std::nullopt;
}

} // namespace kumo::renderer::detail
