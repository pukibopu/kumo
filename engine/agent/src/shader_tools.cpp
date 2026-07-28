#include <kumo/agent/shader_tools.h>

#include <kumo/agent/entity_id.h>
#include <kumo/core/assert.h>
#include <kumo/core/file.h>
#include <kumo/core/log.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kumo::agent {

namespace {

using nlohmann::json;

constexpr int kMaxAttempts = 5;

struct EntityLookup {
    scene::EntityId id;
    scene::Entity* entity = nullptr;
};

std::expected<EntityLookup, std::string> findEntity(scene::Scene& scene, const json& args) {
    const auto it = args.find("entity_id");
    if (it == args.end() || !it->is_string()) {
        return std::unexpected(errorJson("entity_id (string) is required"));
    }
    const std::string text = it->get<std::string>();
    const std::optional<scene::EntityId> id = parseEntityId(text);
    if (!id.has_value()) {
        return std::unexpected(
            errorJson(std::format("entity_id '{}' is not of the form 'index:generation'", text)));
    }
    scene::Entity* entity = scene.entities.get(*id);
    if (entity == nullptr) {
        return std::unexpected(errorJson(std::format("entity not found: {}", text)));
    }
    return EntityLookup{*id, entity};
}

json parseArgs(std::string_view argsJson) {
    if (argsJson.empty()) {
        return json::object();
    }
    // The registry already rejected non-object payloads (ADR 0028).
    return json::parse(argsJson, nullptr, false);
}

std::string shaderRead(const ShaderToolContext& context, const json& args) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    const std::int32_t materialIndex = lookup->entity->materialIndex;

    const std::string* customSource = nullptr;
    if (materialIndex >= 0 && context.shaderSource) {
        customSource = context.shaderSource(static_cast<std::uint32_t>(materialIndex));
    }
    if (customSource != nullptr) {
        return json{{"status", "ok"},
                    {"customized", true},
                    {"material_index", materialIndex},
                    {"source", *customSource}}
            .dump();
    }

    const auto text = readTextFile(context.templatePath);
    if (!text.has_value()) {
        return errorJson(std::format("cannot read shader template: {}", text.error()));
    }
    return json{{"status", "ok"},
                {"customized", false},
                {"material_index", materialIndex},
                {"source", *text}}
        .dump();
}

json compileErrorsJson(const std::vector<shaderc::CompileError>& errors) {
    json out = json::array();
    for (const shaderc::CompileError& error : errors) {
        out.push_back({{"file", error.file},
                       {"line", error.line},
                       {"message", error.message},
                       {"second_stage", error.secondStage}});
    }
    return out;
}

std::string shaderWrite(const ShaderToolContext& context, const json& args) {
    const auto sourceIt = args.find("source");
    if (sourceIt == args.end() || !sourceIt->is_string()) {
        return errorJson("source (string) is required");
    }
    const std::string source = sourceIt->get<std::string>();
    if (source.empty()) {
        return errorJson("source must not be empty");
    }

    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    if (lookup->entity->materialIndex < 0) {
        return errorJson("entity has no material; set a material parameter first to allocate one");
    }
    if (!context.setShader) {
        return errorJson("renderer unavailable");
    }
    const auto materialIndex = static_cast<std::uint32_t>(lookup->entity->materialIndex);

    int& attempts = (*context.failureCounts)[lookup->entity->materialIndex];
    if (attempts >= kMaxAttempts) {
        return errorJson(
            std::format("attempt limit reached ({}) for this material; stop and report the compile "
                        "problem to the user",
                        kMaxAttempts));
    }

    const auto compiled = context.setShader(materialIndex, source);
    if (!compiled.has_value()) {
        ++attempts;
        return json{{"status", "error"},
                    {"message", "shader compile failed"},
                    {"attempts_used", attempts},
                    {"errors", compileErrorsJson(compiled.error())}}
            .dump();
    }
    attempts = 0;

    std::size_t sharers = 0;
    context.scene->entities.forEach([&](scene::EntityId, const scene::Entity& other) {
        if (other.materialIndex == lookup->entity->materialIndex) {
            ++sharers;
        }
    });

    // Persistence is best-effort: the shader is already installed by setShader
    // above, so a disk failure here must not turn the tool result into an error.
    std::error_code ec;
    std::filesystem::create_directories(context.generatedDir, ec);
    const std::filesystem::path savedTo =
        context.generatedDir / ("material_" + std::to_string(materialIndex) + ".frag");
    std::ofstream out(savedTo, std::ios::trunc);
    if (out) {
        out << source;
    }
    if (!out) {
        logError("shader_write: failed to save {}", savedTo.string());
    }

    json result{
        {"status", "ok"}, {"material_index", materialIndex}, {"saved_to", savedTo.string()}};
    if (sharers > 1) {
        result["entities_sharing_material"] = sharers;
    }
    return result.dump();
}

} // namespace

void registerShaderTools(ToolRegistry& registry, ShaderToolContext context) {
    KUMO_ASSERT(context.scene != nullptr);
    if (!context.failureCounts) {
        context.failureCounts = std::make_shared<std::unordered_map<std::int32_t, int>>();
    }

    auto add = [&](const char* name, const char* description, const char* schema, auto handler) {
        registry.add({.name = name,
                      .description = description,
                      .parametersSchema = schema,
                      .destructive = false},
                     [context, handler](std::string_view argsJson) {
                         return handler(context, parseArgs(argsJson));
                     });
    };

    add("shader_read",
        "Read the effective fragment shader for an entity's material: the customized source if "
        "one is installed, else the shared pbr template. Edit and submit the FULL file via "
        "shader_write.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string","description":"Id from scene_list or scene_add_entity"}},
"required":["entity_id"]})",
        [](const ShaderToolContext& ctx, const json& args) { return shaderRead(ctx, args); });

    add("shader_write",
        "Replace the FULL fragment shader source for an entity's material (only that material is "
        "affected). Compile errors come back structured with file/line; fix and retry, at most 5 "
        "attempts.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"source":{"type":"string","description":"Complete fragment shader source"}},
"required":["entity_id","source"]})",
        [](const ShaderToolContext& ctx, const json& args) { return shaderWrite(ctx, args); });
}

} // namespace kumo::agent
