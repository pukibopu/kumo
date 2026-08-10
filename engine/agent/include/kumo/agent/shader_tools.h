#pragma once

#include <kumo/agent/tool_registry.h>
#include <kumo/shaderc/compiler.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kumo::scene {
class Scene;
}

namespace kumo::agent {

namespace search {
struct SearchCache;
}

// Renderer-decoupled hooks so CPU-only tests can inject fakes; the composition
// root binds these to ForwardRenderer. The scene resolves entity ids to
// material indices.
// Renderer-free mirror of ForwardRenderer::SurfaceParam (MD): offsets are
// engine-computed at splice/load time, values cached for introspection.
struct SurfaceParamSpec {
    std::string name;
    bool isVec4 = false;
    std::uint32_t offset = 0;
    float value[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

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

    // Surface-function path (MD); null callbacks report the feature unsupported.
    std::filesystem::path surfaceTemplatePath; // shaders/pbr_surface_template.frag
    std::filesystem::path recipesDir;          // shaders/recipes
    // ForwardRenderer::setMaterialSurfaceParams shaped.
    std::function<bool(std::uint32_t materialIndex, const std::vector<SurfaceParamSpec>& params)>
        setSurfaceParams;
    // ForwardRenderer::setMaterialSurfaceParam shaped; values carries 1 or 4 floats.
    std::function<bool(std::uint32_t materialIndex, const std::string& name,
                       const std::vector<float>& values)>
        setSurfaceParam;
    // ForwardRenderer::materialSurfaceParams shaped; empty when none installed.
    std::function<std::vector<SurfaceParamSpec>(std::uint32_t materialIndex)> surfaceParams;

    // material_recipe_search (MR): the asset library root holding index.json,
    // plus the same embedder/cache the scene registry's asset_search uses
    // (the composition root passes one shared SearchCache instance to both).
    std::filesystem::path assetDir;
    std::function<std::optional<std::vector<float>>(std::string_view)> embedQuery;
    std::shared_ptr<search::SearchCache> searchCache;
};

// Registers shader_read, surface_write, shader_set_param, recipe_list,
// material_recipe_search, shader_apply_recipe and shader_write_full (ADR
// 0011/0029; MD adds the surface path, MR the recipe search, shader_write
// stays as a deprecated alias). The context is copied into the handlers;
// everything it references must outlive the registry.
void registerShaderTools(ToolRegistry& registry, ShaderToolContext context);

} // namespace kumo::agent
