#pragma once

#include <kumo/agent/tool_registry.h>
#include <kumo/shaderc/compiler.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kumo::scene {
class Scene;
}

namespace kumo::agent {

// Renderer-decoupled hooks so CPU-only tests can inject fakes; the composition
// root binds these to ForwardRenderer. The scene resolves entity ids to
// material indices.
struct ShaderToolContext {
    scene::Scene* scene = nullptr;
    // ForwardRenderer::setMaterialShader shaped: install a custom fragment shader.
    std::function<std::expected<void, std::vector<shaderc::CompileError>>(
        std::uint32_t materialIndex, std::string_view source)>
        setShader;
    // ForwardRenderer::materialShaderSource shaped: null when uncustomized.
    std::function<const std::string*(std::uint32_t materialIndex)> shaderSource;
    std::filesystem::path templatePath; // shaders/pbr.frag
    std::filesystem::path generatedDir; // shaders/generated
    // Consecutive shader_write compile-failure count per material index (attempt
    // cap, docs/agents.md); shared so it survives across handler invocations.
    // registerShaderTools default-constructs it when left null.
    std::shared_ptr<std::unordered_map<std::int32_t, int>> failureCounts;
};

// Registers shader_read and shader_write (ADR 0011/0029). The context is
// copied into the handlers; everything it references must outlive the registry.
void registerShaderTools(ToolRegistry& registry, ShaderToolContext context);

} // namespace kumo::agent
