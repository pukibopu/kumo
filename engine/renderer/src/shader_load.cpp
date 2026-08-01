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

gpu::ShaderStage toGpuStage(shaderc::Stage stage) {
    switch (stage) {
    case shaderc::Stage::Vertex:
        return gpu::ShaderStage::Vertex;
    case shaderc::Stage::Fragment:
        return gpu::ShaderStage::Fragment;
    case shaderc::Stage::Compute:
        return gpu::ShaderStage::Compute;
    }
    return gpu::ShaderStage::None;
}

std::optional<gpu::BindingType> toBindingType(const std::string& type) {
    if (type == "uniform_buffer") {
        return gpu::BindingType::UniformBuffer;
    }
    if (type == "storage_buffer") {
        return gpu::BindingType::StorageBuffer;
    }
    if (type == "sampled_texture") {
        return gpu::BindingType::Texture;
    }
    if (type == "storage_texture") {
        return gpu::BindingType::StorageTexture;
    }
    if (type == "sampler") {
        return gpu::BindingType::Sampler;
    }
    return std::nullopt;
}

struct MergedBinding {
    gpu::BindingType type = gpu::BindingType::UniformBuffer;
    gpu::ShaderStage visibility = gpu::ShaderStage::None;
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

std::optional<CompiledStage> loadStage(gpu::Device& device, const char* file,
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

    gpu::Ptr<gpu::ShaderModule> module = device.createShaderModule({
        .stage = toGpuStage(stage),
        .mslSource = compiled->msl,
        .entryPoint = compiled->mslEntryPoint,
    });
    if (!module) {
        return std::nullopt;
    }
    return CompiledStage{std::move(module), std::move(compiled->reflection)};
}

std::vector<gpu::Ptr<gpu::BindGroupLayout>>
layoutsFromReflection(gpu::Device& device, std::span<const StageReflection> stages) {
    const auto merged = mergeBindings(stages);
    std::uint32_t setCount = 0;
    for (const auto& [key, binding] : merged) {
        setCount = std::max(setCount, key.first + 1);
    }

    std::vector<gpu::Ptr<gpu::BindGroupLayout>> layouts(setCount);
    for (std::uint32_t set = 0; set < setCount; ++set) {
        gpu::BindGroupLayoutDesc desc;
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
        if (sharedBinding->members != customBinding->members) {
            return std::format("set {} binding {} member names changed", set, bindingIndex);
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

std::uint32_t factorPrefixSize(const shaderc::ReflectionBinding& factors) {
    constexpr std::uint32_t kPrefixSize = 48; // baseColor + metallicRoughness + emissive
    constexpr std::uint32_t kFullSize = 64;   // + uvTiling
    const bool hasUvTiling = factors.members.size() >= 4 && factors.members[3] == "uvTiling";
    return std::min(hasUvTiling ? kFullSize : kPrefixSize, factors.bufferSize);
}

bool sourceReferencesTime(std::string_view source) {
    return source.find("timeParams") != std::string_view::npos;
}

} // namespace kumo::renderer::detail
