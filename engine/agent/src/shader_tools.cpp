#include <kumo/agent/shader_tools.h>

#include "surface_template.h"

#include <kumo/agent/entity_id.h>
#include <kumo/core/assert.h>
#include <kumo/core/asset_name.h>
#include <kumo/core/file.h>
#include <kumo/core/log.h>
#include <kumo/scene/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <span>
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

// Shared by surface_write and shader_apply_recipe: splice, compile-install,
// register the layout+values, persist the full source. Returns the tool JSON.
std::string installSurface(const ShaderToolContext& context, const json& args,
                           std::string_view functionSource,
                           std::span<const surface::ParamDecl> decls) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    if (lookup->entity->materialIndex < 0) {
        return errorJson("entity has no material; set a material parameter first to allocate one");
    }
    if (!context.setShader || !context.setSurfaceParams) {
        return errorJson("renderer unavailable");
    }
    const auto materialIndex = static_cast<std::uint32_t>(lookup->entity->materialIndex);

    const auto templateText = readTextFile(context.surfaceTemplatePath);
    if (!templateText.has_value()) {
        return errorJson(std::format("cannot read surface template: {}", templateText.error()));
    }
    const auto spliced = surface::spliceSurface(*templateText, functionSource, decls);
    if (!spliced.has_value()) {
        return errorJson(spliced.error());
    }

    int& attempts = (*context.failureCounts)[lookup->entity->materialIndex];
    if (attempts >= kMaxAttempts) {
        return errorJson(
            std::format("attempt limit reached ({}) for this material; stop and report the compile "
                        "problem to the user",
                        kMaxAttempts));
    }
    const auto compiled = context.setShader(materialIndex, spliced->source);
    if (!compiled.has_value()) {
        ++attempts;
        // Lines map back onto the function text; anything else is a template
        // zone error the model cannot fix by editing its function.
        json errors = json::array();
        for (const shaderc::CompileError& error : compiled.error()) {
            json entry{{"message", error.message}, {"second_stage", error.secondStage}};
            if (const std::optional<int> line = surface::functionLine(*spliced, error.line)) {
                entry["function_line"] = *line;
            } else {
                entry["template_error"] = true;
            }
            errors.push_back(std::move(entry));
        }
        return json{{"status", "error"},
                    {"message", "surface function compile failed"},
                    {"attempts_used", attempts},
                    {"errors", std::move(errors)}}
            .dump();
    }
    attempts = 0;

    std::vector<SurfaceParamSpec> specs;
    for (std::size_t i = 0; i < spliced->layout.size(); ++i) {
        SurfaceParamSpec spec{.name = spliced->layout[i].name,
                              .isVec4 = spliced->layout[i].isVec4,
                              .offset = spliced->layout[i].offset};
        std::copy(std::begin(decls[i].value), std::end(decls[i].value), spec.value);
        specs.push_back(std::move(spec));
    }
    if (!context.setSurfaceParams(materialIndex, specs)) {
        return errorJson("surface parameter install failed");
    }

    std::error_code ec;
    std::filesystem::create_directories(context.generatedDir, ec);
    const std::filesystem::path savedTo =
        context.generatedDir / ("material_" + std::to_string(materialIndex) + ".frag");
    std::ofstream out(savedTo, std::ios::trunc);
    if (out) {
        out << spliced->source;
    }
    if (!out) {
        logError("surface_write: failed to save {}", savedTo.string());
    }

    json paramsJson = json::array();
    for (std::size_t i = 0; i < decls.size(); ++i) {
        json value = json::array();
        for (std::size_t c = 0; c < (decls[i].isVec4 ? 4u : 1u); ++c) {
            value.push_back(decls[i].value[c]);
        }
        paramsJson.push_back({{"name", decls[i].name},
                              {"type", decls[i].isVec4 ? "vec4" : "float"},
                              {"value", std::move(value)}});
    }
    return json{{"status", "ok"},
                {"material_index", materialIndex},
                {"params", std::move(paramsJson)},
                {"saved_to", savedTo.string()}}
        .dump();
}

// Accepts {name, type: "float"|"vec4", value: number|[..4]}; `value` optional
// (defaults to zero).
std::expected<std::vector<surface::ParamDecl>, std::string> parseParamDecls(const json& args) {
    std::vector<surface::ParamDecl> decls;
    const auto it = args.find("params");
    if (it == args.end()) {
        return decls;
    }
    if (!it->is_array()) {
        return std::unexpected("params must be an array");
    }
    for (const json& entry : *it) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("type") || !entry["type"].is_string()) {
            return std::unexpected("each param needs name and type");
        }
        surface::ParamDecl decl;
        decl.name = entry["name"].get<std::string>();
        const std::string type = entry["type"].get<std::string>();
        if (type != "float" && type != "vec4") {
            return std::unexpected(
                std::format("param '{}': type must be float or vec4", decl.name));
        }
        decl.isVec4 = type == "vec4";
        if (const auto valueIt = entry.find("value"); valueIt != entry.end()) {
            if (decl.isVec4) {
                if (!valueIt->is_array() || valueIt->size() != 4) {
                    return std::unexpected(
                        std::format("param '{}': vec4 value must be [x,y,z,w]", decl.name));
                }
                for (std::size_t c = 0; c < 4; ++c) {
                    if (!(*valueIt)[c].is_number()) {
                        return std::unexpected(
                            std::format("param '{}': values must be numbers", decl.name));
                    }
                    decl.value[c] = (*valueIt)[c].get<float>();
                }
            } else {
                if (!valueIt->is_number()) {
                    return std::unexpected(
                        std::format("param '{}': float value must be a number", decl.name));
                }
                decl.value[0] = valueIt->get<float>();
            }
        }
        decls.push_back(std::move(decl));
    }
    return decls;
}

std::string surfaceWrite(const ShaderToolContext& context, const json& args) {
    const auto functionIt = args.find("function");
    if (functionIt == args.end() || !functionIt->is_string() ||
        functionIt->get<std::string>().empty()) {
        return errorJson("function (string) is required");
    }
    const auto decls = parseParamDecls(args);
    if (!decls.has_value()) {
        return errorJson(decls.error());
    }
    return installSurface(context, args, functionIt->get<std::string>(), *decls);
}

std::string shaderSetParam(const ShaderToolContext& context, const json& args) {
    auto lookup = findEntity(*context.scene, args);
    if (!lookup.has_value()) {
        return lookup.error();
    }
    if (lookup->entity->materialIndex < 0) {
        return errorJson("entity has no material");
    }
    if (!context.setSurfaceParam || !context.surfaceParams) {
        return errorJson("renderer unavailable");
    }
    const auto nameIt = args.find("name");
    if (nameIt == args.end() || !nameIt->is_string()) {
        return errorJson("name (string) is required");
    }
    const auto valueIt = args.find("value");
    if (valueIt == args.end()) {
        return errorJson("value (number or [x,y,z,w]) is required");
    }
    std::vector<float> values;
    if (valueIt->is_number()) {
        values.push_back(valueIt->get<float>());
    } else if (valueIt->is_array() && valueIt->size() == 4) {
        for (const json& entry : *valueIt) {
            if (!entry.is_number()) {
                return errorJson("value entries must be numbers");
            }
            values.push_back(entry.get<float>());
        }
    } else {
        return errorJson("value must be a number (float param) or [x,y,z,w] (vec4 param)");
    }

    const auto materialIndex = static_cast<std::uint32_t>(lookup->entity->materialIndex);
    const std::string name = nameIt->get<std::string>();
    if (!context.setSurfaceParam(materialIndex, name, values)) {
        std::string available;
        for (const SurfaceParamSpec& spec : context.surfaceParams(materialIndex)) {
            available += available.empty() ? spec.name : ", " + spec.name;
        }
        return errorJson(
            available.empty()
                ? "this material has no surface parameters; use surface_write first"
                : std::format("no parameter '{}' with that arity; available: {}", name, available));
    }
    return json{{"status", "ok"}, {"material_index", materialIndex}, {"name", name}}.dump();
}

// Recipe metadata (<name>.json next to <name>.frag); sources never leave disk
// through recipe_list (MR anti-goal).
std::string recipeList(const ShaderToolContext& context, const json&) {
    if (context.recipesDir.empty()) {
        return errorJson("no recipe library configured");
    }
    json recipes = json::array();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(context.recipesDir, ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const auto text = readTextFile(entry.path());
        if (!text.has_value()) {
            continue;
        }
        json parsed = json::parse(*text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            logError("recipe_list: {} is not valid JSON", entry.path().string());
            continue;
        }
        parsed["name"] = entry.path().stem().string();
        recipes.push_back(std::move(parsed));
    }
    std::sort(recipes.begin(), recipes.end(),
              [](const json& a, const json& b) { return a["name"] < b["name"]; });
    return json{{"status", "ok"}, {"recipes", std::move(recipes)}}.dump();
}

std::string shaderApplyRecipe(const ShaderToolContext& context, const json& args) {
    const auto nameIt = args.find("name");
    if (nameIt == args.end() || !nameIt->is_string()) {
        return errorJson("name (string) is required");
    }
    const std::string name = nameIt->get<std::string>();
    if (!isPlainAssetName(name)) {
        return errorJson("recipe names are plain names from recipe_list, not paths");
    }
    const auto functionText = readTextFile(context.recipesDir / (name + ".frag"));
    const auto metaText = readTextFile(context.recipesDir / (name + ".json"));
    if (!functionText.has_value() || !metaText.has_value()) {
        return errorJson(std::format("unknown recipe '{}'; call recipe_list first", name));
    }
    const json meta = json::parse(*metaText, nullptr, false);
    if (meta.is_discarded() || !meta.is_object()) {
        return errorJson(std::format("recipe '{}' metadata is corrupt", name));
    }

    // Defaults from the recipe metadata, then per-call overrides by name.
    auto decls = parseParamDecls(meta);
    if (!decls.has_value()) {
        return errorJson(std::format("recipe '{}': {}", name, decls.error()));
    }
    if (const auto overridesIt = args.find("params");
        overridesIt != args.end() && overridesIt->is_object()) {
        for (const auto& [key, value] : overridesIt->items()) {
            const auto decl =
                std::find_if(decls->begin(), decls->end(),
                             [&key](const surface::ParamDecl& d) { return d.name == key; });
            if (decl == decls->end()) {
                return errorJson(std::format("recipe '{}' has no param '{}'", name, key));
            }
            if (decl->isVec4) {
                if (!value.is_array() || value.size() != 4) {
                    return errorJson(std::format("param '{}' wants [x,y,z,w]", key));
                }
                for (std::size_t c = 0; c < 4; ++c) {
                    if (!value[c].is_number()) {
                        return errorJson(std::format("param '{}' values must be numbers", key));
                    }
                    decl->value[c] = value[c].get<float>();
                }
            } else {
                if (!value.is_number()) {
                    return errorJson(std::format("param '{}' wants a number", key));
                }
                decl->value[0] = value.get<float>();
            }
        }
    }
    return installSurface(context, args, *functionText, *decls);
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
        "one is installed, else the shared pbr template. Most material work should go through "
        "surface_write instead; shader_write_full replaces the whole file.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string","description":"Id from scene_list or scene_add_entity"}},
"required":["entity_id"]})",
        [](const ShaderToolContext& ctx, const json& args) { return shaderRead(ctx, args); });

    add("surface_write",
        "PREFERRED way to customize a material: submit only a surface function 'void "
        "kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i)' (helpers and for-loops "
        "allowed; no uniforms, no main, no while/do) plus optional named params. The engine "
        "splices it over the standard PBR lighting; params become live-tunable via "
        "shader_set_param and the Inspector.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"function":{"type":"string","description":"The kumoSurface function, plus any helper functions it needs"},
"params":{"type":"array","maxItems":16,"items":{"type":"object","properties":{
"name":{"type":"string"},
"type":{"type":"string","enum":["float","vec4"]},
"value":{"description":"Default: number for float, [x,y,z,w] for vec4"}},
"required":["name","type"]}}},
"required":["entity_id","function"]})",
        [](const ShaderToolContext& ctx, const json& args) { return surfaceWrite(ctx, args); });

    add("shader_set_param",
        "Set one named parameter of a material's surface function (installed by surface_write "
        "or shader_apply_recipe) without recompiling.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"name":{"type":"string"},
"value":{"description":"number for float params, [x,y,z,w] for vec4 params"}},
"required":["entity_id","name","value"]})",
        [](const ShaderToolContext& ctx, const json& args) { return shaderSetParam(ctx, args); });

    add("recipe_list",
        "List the material recipe library: name, description and tunable params per recipe. "
        "Apply one with shader_apply_recipe.",
        R"({"type":"object","properties":{}})",
        [](const ShaderToolContext& ctx, const json& args) { return recipeList(ctx, args); });

    add("shader_apply_recipe",
        "Apply a library recipe (name from recipe_list) to an entity's material, optionally "
        "overriding its params; same surface pipeline as surface_write.",
        R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"name":{"type":"string","description":"Recipe name from recipe_list"},
"params":{"type":"object","description":"Optional per-param overrides, keyed by param name"}},
"required":["entity_id","name"]})",
        [](const ShaderToolContext& ctx, const json& args) {
            return shaderApplyRecipe(ctx, args);
        });

    const char* fullDescription =
        "ADVANCED: replace the FULL fragment shader source for an entity's material (only that "
        "material is affected). Only for custom lighting or fully stylized looks the surface "
        "path cannot express, or when the user explicitly asks. Compile errors come back "
        "structured with file/line; fix and retry, at most 5 attempts.";
    const char* fullSchema = R"({"type":"object","properties":{
"entity_id":{"type":"string"},
"source":{"type":"string","description":"Complete fragment shader source"}},
"required":["entity_id","source"]})";
    add("shader_write_full", fullDescription, fullSchema,
        [](const ShaderToolContext& ctx, const json& args) { return shaderWrite(ctx, args); });
    // Deprecated alias, kept for one release for older MCP scripts.
    add("shader_write", fullDescription, fullSchema,
        [](const ShaderToolContext& ctx, const json& args) { return shaderWrite(ctx, args); });
}

} // namespace kumo::agent
